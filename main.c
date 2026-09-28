#define _GNU_SOURCE
#include "launcher.h"
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define NUM_BUFFERS 1

typedef struct {
    struct wl_buffer *wl_buffer;
    uint32_t *data;
    uint32_t width;
    uint32_t height;
    size_t size;
    bool busy;
} ShmBuffer;

typedef struct {
    LauncherState state;

    /* Wayland globals */
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct zwlr_layer_shell_v1 *layer_shell;

    /* Surface & Layer surface */
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    uint32_t surface_width;
    uint32_t surface_height;
    bool configured;

    /* Buffers */
    ShmBuffer buffers[NUM_BUFFERS];

    /* Input devices */
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;

    /* XKB Keyboard state */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    uint32_t current_mods;

    /* Keyboard repeat */
    int repeat_timer_fd;
    int32_t repeat_rate;     /* characters per second */
    int32_t repeat_delay_ms; /* delay before repeat in milliseconds */
    uint32_t repeat_key;     /* raw key code */
    xkb_keysym_t repeat_sym;
    char repeat_utf8[32];

    /* Pointer state */
    int pointer_x;
    int pointer_y;

    /* Lock file */
    int lock_fd;
    char lock_path[PATH_MAX];
} WaylandApp;

static WaylandApp app;

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */
static void render_and_commit(WaylandApp *app);
static void arm_repeat_timer(WaylandApp *app, uint32_t key, xkb_keysym_t sym, const char *utf8);
static void disarm_repeat_timer(WaylandApp *app);

/* -------------------------------------------------------------------------
 * Lock file management: Single-instance toggle
 * ------------------------------------------------------------------------- */
static int acquire_single_instance_lock(WaylandApp *app) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !runtime_dir[0]) {
        runtime_dir = "/tmp";
    }

    snprintf(app->lock_path, sizeof(app->lock_path), "%s/launcher.lock", runtime_dir);

    app->lock_fd = open(app->lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (app->lock_fd < 0) {
        perror("open lock file");
        return -1;
    }

    if (flock(app->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            /* Another instance is running: read its PID and send SIGTERM to toggle/close */
            char buf[32];
            ssize_t n = pread(app->lock_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                pid_t other_pid = (pid_t)atoi(buf);
                if (other_pid > 0) {
                    kill(other_pid, SIGTERM);
                }
            }
            close(app->lock_fd);
            app->lock_fd = -1;
            return 1; /* Closed existing instance cleanly */
        }
        perror("flock");
        close(app->lock_fd);
        app->lock_fd = -1;
        return -1;
    }

    /* Lock acquired: write current PID */
    if (ftruncate(app->lock_fd, 0) == 0) {
        char pid_buf[32];
        int len = snprintf(pid_buf, sizeof(pid_buf), "%d\n", (int)getpid());
        if (len > 0) {
            pwrite(app->lock_fd, pid_buf, (size_t)len, 0);
        }
    }

    return 0;
}

static void release_single_instance_lock(WaylandApp *app) {
    if (app->lock_fd >= 0) {
        flock(app->lock_fd, LOCK_UN);
        close(app->lock_fd);
        app->lock_fd = -1;
        unlink(app->lock_path);
    }
}

/* -------------------------------------------------------------------------
 * Shared Memory (wl_shm) Buffer Management
 * ------------------------------------------------------------------------- */
static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
    (void)wl_buffer;
    ShmBuffer *buf = (ShmBuffer *)data;
    buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void destroy_shm_buffer(ShmBuffer *buf) {
    if (buf->wl_buffer) {
        wl_buffer_destroy(buf->wl_buffer);
        buf->wl_buffer = NULL;
    }
    if (buf->data && buf->data != MAP_FAILED) {
        munmap(buf->data, buf->size);
        buf->data = NULL;
    }
    buf->width = 0;
    buf->height = 0;
    buf->size = 0;
    buf->busy = false;
}

static int create_shm_buffer(struct wl_shm *shm, ShmBuffer *buf, uint32_t width, uint32_t height) {
    destroy_shm_buffer(buf);

    uint32_t stride = width * 4;
    size_t size = (size_t)stride * height;

    int fd = memfd_create("launcher-shm", MFD_CLOEXEC);
    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }

    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *wl_buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!wl_buf) {
        munmap(data, size);
        return -1;
    }

    buf->data = (uint32_t *)data;
    buf->width = width;
    buf->height = height;
    buf->size = size;
    buf->busy = false;
    buf->wl_buffer = wl_buf;

    wl_buffer_add_listener(wl_buf, &buffer_listener, buf);
    return 0;
}

static ShmBuffer *get_next_buffer(WaylandApp *app, uint32_t width, uint32_t height) {
    ShmBuffer *selected = NULL;

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (!app->buffers[i].busy) {
            selected = &app->buffers[i];
            break;
        }
    }

    if (!selected) {
        /* Both busy: fallback to buffer 0 */
        selected = &app->buffers[0];
    }

    if (selected->width != width || selected->height != height || !selected->wl_buffer) {
        if (create_shm_buffer(app->shm, selected, width, height) < 0) {
            return NULL;
        }
    }

    return selected;
}

/* -------------------------------------------------------------------------
 * Layer Surface configure & listener
 * ------------------------------------------------------------------------- */
static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *layer_surface,
                                    uint32_t serial,
                                    uint32_t width,
                                    uint32_t height) {
    WaylandApp *app = (WaylandApp *)data;

    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    if (width == 0 || height == 0) {
        width = 1920;
        height = 1080;
    }

    app->surface_width = width;
    app->surface_height = height;
    app->state.screen_w = width;
    app->state.screen_h = height;
    app->configured = true;

    ui_compute_layout(&app->state, width, height);
    app->state.needs_redraw = true;

    render_and_commit(app);
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *layer_surface) {
    (void)layer_surface;
    WaylandApp *app = (WaylandApp *)data;
    app->state.running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* -------------------------------------------------------------------------
 * wl_keyboard listener & Key repeat
 * ------------------------------------------------------------------------- */
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
    (void)keyboard;
    WaylandApp *app = (WaylandApp *)data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    if (app->xkb_keymap) {
        xkb_keymap_unref(app->xkb_keymap);
        app->xkb_keymap = NULL;
    }
    if (app->xkb_state) {
        xkb_state_unref(app->xkb_state);
        app->xkb_state = NULL;
    }

    app->xkb_keymap = xkb_keymap_new_from_string(app->xkb_ctx, map_str,
                                                  XKB_KEYMAP_FORMAT_TEXT_V1,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size);
    close(fd);

    if (app->xkb_keymap) {
        app->xkb_state = xkb_state_new(app->xkb_keymap);
    }
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface) {
    (void)keyboard;
    (void)serial;
    (void)surface;
    WaylandApp *app = (WaylandApp *)data;
    disarm_repeat_timer(app);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state) {
    (void)keyboard;
    (void)serial;
    (void)time;
    WaylandApp *app = (WaylandApp *)data;

    if (!app->xkb_state || !app->xkb_keymap) return;

    uint32_t keycode = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(app->xkb_state, keycode);
    char utf8[32] = {0};
    xkb_state_key_get_utf8(app->xkb_state, keycode, utf8, sizeof(utf8));

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        bool handled = vim_handle_key(&app->state, sym, utf8, app->current_mods);
        if (handled) {
            app->state.needs_redraw = true;
        }

        if (app->repeat_rate > 0 && xkb_keymap_key_repeats(app->xkb_keymap, keycode)) {
            arm_repeat_timer(app, key, sym, utf8);
        } else {
            disarm_repeat_timer(app);
        }
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (app->repeat_key == key) {
            disarm_repeat_timer(app);
        }
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group) {
    (void)keyboard;
    (void)serial;
    WaylandApp *app = (WaylandApp *)data;

    if (!app->xkb_state) return;

    xkb_state_update_mask(app->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);

    app->current_mods = 0;
    if (xkb_state_mod_name_is_active(app->xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) {
        app->current_mods |= MOD_SHIFT;
    }
    if (xkb_state_mod_name_is_active(app->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE)) {
        app->current_mods |= MOD_CTRL;
    }
    if (xkb_state_mod_name_is_active(app->xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE)) {
        app->current_mods |= MOD_ALT;
    }
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
    (void)keyboard;
    WaylandApp *app = (WaylandApp *)data;
    app->repeat_rate = rate;
    app->repeat_delay_ms = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void arm_repeat_timer(WaylandApp *app, uint32_t key, xkb_keysym_t sym, const char *utf8) {
    app->repeat_key = key;
    app->repeat_sym = sym;
    if (utf8) {
        strncpy(app->repeat_utf8, utf8, sizeof(app->repeat_utf8) - 1);
        app->repeat_utf8[sizeof(app->repeat_utf8) - 1] = '\0';
    } else {
        app->repeat_utf8[0] = '\0';
    }

    if (app->repeat_timer_fd < 0 || app->repeat_rate <= 0) return;

    struct itimerspec its;
    its.it_value.tv_sec = app->repeat_delay_ms / 1000;
    its.it_value.tv_nsec = (app->repeat_delay_ms % 1000) * 1000000LL;

    int64_t interval_ns = (1000000000LL) / app->repeat_rate;
    its.it_interval.tv_sec = interval_ns / 1000000000LL;
    its.it_interval.tv_nsec = interval_ns % 1000000000LL;

    timerfd_settime(app->repeat_timer_fd, 0, &its, NULL);
}

static void disarm_repeat_timer(WaylandApp *app) {
    app->repeat_key = 0;
    app->repeat_sym = XKB_KEY_NoSymbol;
    app->repeat_utf8[0] = '\0';

    if (app->repeat_timer_fd >= 0) {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        timerfd_settime(app->repeat_timer_fd, 0, &its, NULL);
    }
}

/* -------------------------------------------------------------------------
 * wl_pointer listener
 * ------------------------------------------------------------------------- */
static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)serial;
    (void)surface;
    WaylandApp *app = (WaylandApp *)data;
    app->pointer_x = wl_fixed_to_int(sx);
    app->pointer_y = wl_fixed_to_int(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)time;
    WaylandApp *app = (WaylandApp *)data;

    int new_x = wl_fixed_to_int(sx);
    int new_y = wl_fixed_to_int(sy);
    if (new_x == app->pointer_x && new_y == app->pointer_y) {
        return;
    }
    app->pointer_x = new_x;
    app->pointer_y = new_y;

    int idx = ui_grid_index_at(&app->state, app->pointer_x, app->pointer_y);
    if (idx >= 0 && idx != app->state.selected_idx) {
        app->state.selected_idx = idx;
        app->state.focus = FOCUS_GRID;
        app->state.needs_redraw = true;
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
    (void)pointer;
    (void)serial;
    (void)time;
    WaylandApp *app = (WaylandApp *)data;

    /* 0x110 = BTN_LEFT */
    if (button == 0x110 && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        int idx = ui_grid_index_at(&app->state, app->pointer_x, app->pointer_y);
        if (idx >= 0 && (size_t)idx < app->state.filtered_count) {
            int app_idx = app->state.filtered[idx];
            cache_record_launch(&app->state, app_idx);
            launch_desktop_app(&app->state.apps[app_idx]);
            app->state.running = false;
        } else if (ui_is_in_textbox(&app->state, app->pointer_x, app->pointer_y)) {
            app->state.error_msg[0] = '\0';
            app->state.focus = FOCUS_TEXTBOX;
            app->state.mode = VIM_MODE_INSERT;
            app->state.needs_redraw = true;
        } else {
            /* Clicked backdrop outside textbox and grid: dismiss overlay */
            app->state.running = false;
        }
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)pointer;
    (void)time;
    WaylandApp *app = (WaylandApp *)data;

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        int cols = app->state.layout.cols > 0 ? app->state.layout.cols : 1;
        int total_rows = ((int)app->state.filtered_count + cols - 1) / cols;
        int max_scroll = total_rows - app->state.layout.visible_rows;
        if (max_scroll < 0) max_scroll = 0;

        if (value > 0) {
            /* Scroll down */
            if (app->state.scroll_row < max_scroll) {
                app->state.scroll_row++;
                app->state.needs_redraw = true;
            }
        } else if (value < 0) {
            /* Scroll up */
            if (app->state.scroll_row > 0) {
                app->state.scroll_row--;
                app->state.needs_redraw = true;
            }
        }
    }
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {
    (void)data;
    (void)pointer;
    (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* -------------------------------------------------------------------------
 * wl_seat listener
 * ------------------------------------------------------------------------- */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    WaylandApp *app = (WaylandApp *)data;

    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && app->keyboard) {
        wl_keyboard_destroy(app->keyboard);
        app->keyboard = NULL;
    }

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && app->pointer) {
        wl_pointer_destroy(app->pointer);
        app->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* -------------------------------------------------------------------------
 * Registry listener
 * ------------------------------------------------------------------------- */
static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    (void)version;
    WaylandApp *app = (WaylandApp *)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t v = version < 5 ? version : 5;
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface, v);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        app->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* -------------------------------------------------------------------------
 * Rendering frame
 * ------------------------------------------------------------------------- */
static void render_and_commit(WaylandApp *app) {
    if (!app->configured || app->surface_width == 0 || app->surface_height == 0) {
        return;
    }

    ShmBuffer *buf = get_next_buffer(app, app->surface_width, app->surface_height);
    if (!buf) {
        fprintf(stderr, "Failed to get SHM buffer\n");
        return;
    }

    ui_render_frame(&app->state, buf->data, app->surface_width, app->surface_height);

    buf->busy = true;
    wl_surface_attach(app->surface, buf->wl_buffer, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, app->surface_width, app->surface_height);
    wl_surface_commit(app->surface);

    app->state.needs_redraw = false;
}

/* -------------------------------------------------------------------------
 * CLI Helpers
 * ------------------------------------------------------------------------- */
static void print_usage(const char *progname) {
    printf("Usage: %s [OPTIONS]\n\n"
           "Options:\n"
           "  -h, --help                 Print this help message and exit\n"
           "  -v, --version              Print version information and exit\n"
           "  --rebuild-cache            Force rebuild of desktop applications and icon cache\n"
           "  --dump-apps [query]        List cached apps matching optional query\n"
           "  --check-path <command>     Check if command exists in PATH or is executable\n"
           "  --render-test <out.ppm> [query]  Render UI layout test to PPM image\n"
           "  --test-vim                 Run programmatic test of Vim input mode and motions\n",
           progname);
}

static int do_dump_apps(int argc, char **argv) {
    LauncherState state;
    memset(&state, 0, sizeof(state));

    if (cache_load_or_build(&state, false) != 0) {
        fprintf(stderr, "Failed to load desktop cache\n");
        return 1;
    }

    if (argc >= 3) {
        strncpy(state.query, argv[2], sizeof(state.query) - 1);
        state.query[sizeof(state.query) - 1] = '\0';
        state.query_len = (int)strlen(state.query);
    }

    filter_apps(&state);

    for (size_t i = 0; i < state.filtered_count; i++) {
        int app_idx = state.filtered[i];
        if (app_idx >= 0 && (size_t)app_idx < state.app_count) {
            const AppEntry *app = &state.apps[app_idx];
            printf("%s | %s | %s | %s\n",
                   app->name,
                   app->desktop_id,
                   app->exec_cmd,
                   app->has_icon ? "has_icon" : "no_icon");
        }
    }

    cache_free(&state);
    return 0;
}

static int do_check_path(const char *cmd) {
    PathStatus status = check_path_executable(cmd);
    if (status == PATH_STATUS_VALID) {
        printf("VALID (green)\n");
        return 0;
    } else if (status == PATH_STATUS_EMPTY) {
        printf("EMPTY\n");
        return 0;
    } else {
        printf("INVALID (red)\n");
        return 1;
    }
}

static int do_render_test(const char *output_ppm, const char *query) {
    LauncherState state;
    memset(&state, 0, sizeof(state));

    if (cache_load_or_build(&state, false) != 0) {
        fprintf(stderr, "Failed to load desktop cache for render test\n");
        return 1;
    }

    if (ui_init(NULL, 20) != 0) {
        fprintf(stderr, "Failed to initialize UI renderer\n");
        cache_free(&state);
        return 1;
    }

    vim_init(&state);

    if (query) {
        strncpy(state.query, query, sizeof(state.query) - 1);
        state.query[sizeof(state.query) - 1] = '\0';
        state.query_len = (int)strlen(state.query);
        state.cursor_pos = state.query_len;
    }

    filter_apps(&state);
    state.path_status = check_path_executable(state.query);

    uint32_t width = 1920;
    uint32_t height = 1080;
    ui_compute_layout(&state, width, height);

    uint32_t *argb_buf = calloc((size_t)width * height, sizeof(uint32_t));
    if (!argb_buf) {
        perror("calloc argb_buf");
        ui_cleanup();
        cache_free(&state);
        return 1;
    }

    ui_render_frame(&state, argb_buf, width, height);

    FILE *fp = fopen(output_ppm, "wb");
    if (!fp) {
        perror("fopen output_ppm");
        free(argb_buf);
        ui_cleanup();
        cache_free(&state);
        return 1;
    }

    /* Write binary PPM (P6) header */
    fprintf(fp, "P6\n%u %u\n255\n", width, height);

    /* Convert ARGB8888 premultiplied to RGB888 */
    uint8_t *row_rgb = malloc(width * 3);
    if (!row_rgb) {
        perror("malloc row_rgb");
        fclose(fp);
        free(argb_buf);
        ui_cleanup();
        cache_free(&state);
        return 1;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = argb_buf[y * width + x];
            uint8_t a = (pixel >> 24) & 0xFF;
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;

            if (a > 0 && a < 255) {
                /* Unpremultiply for correct output */
                r = (uint8_t)(((uint32_t)r * 255) / a);
                g = (uint8_t)(((uint32_t)g * 255) / a);
                b = (uint8_t)(((uint32_t)b * 255) / a);
            }

            row_rgb[x * 3 + 0] = r;
            row_rgb[x * 3 + 1] = g;
            row_rgb[x * 3 + 2] = b;
        }
        fwrite(row_rgb, 1, width * 3, fp);
    }

    free(row_rgb);
    fclose(fp);
    free(argb_buf);

    ui_cleanup();
    cache_free(&state);
    return 0;
}

static int do_test_vim(void) {
    LauncherState state;
    memset(&state, 0, sizeof(state));

    /* Initialize vim input state */
    vim_init(&state);
    state.running = true;

    if (state.mode != VIM_MODE_INSERT) {
        fprintf(stderr, "Error: expected initial mode VIM_MODE_INSERT\n");
        return 1;
    }

    /* Type "kill -9 chrome" char by char */
    const char *text = "kill -9 chrome";
    for (size_t i = 0; text[i] != '\0'; i++) {
        char ch_buf[2] = { text[i], '\0' };
        xkb_keysym_t sym = (xkb_keysym_t)text[i];
        if (!vim_handle_key(&state, sym, ch_buf, 0)) {
            fprintf(stderr, "Error: typing '%c' failed\n", text[i]);
            return 1;
        }
    }

    if (strcmp(state.query, "kill -9 chrome") != 0) {
        fprintf(stderr, "Error: query mismatch after typing, got '%s'\n", state.query);
        return 1;
    }
    if (state.query_len != 14 || state.cursor_pos != 14) {
        fprintf(stderr, "Error: unexpected query_len=%d or cursor_pos=%d\n", state.query_len, state.cursor_pos);
        return 1;
    }

    /* Check path_status == PATH_STATUS_VALID ("kill" is a standard executable binary) */
    if (state.path_status != PATH_STATUS_VALID) {
        fprintf(stderr, "Error: expected path_status == PATH_STATUS_VALID for 'kill -9 chrome', got %d\n", state.path_status);
        return 1;
    }

    /* Press Escape to enter VIM_MODE_NORMAL */
    if (!vim_handle_key(&state, XKB_KEY_Escape, NULL, 0)) {
        fprintf(stderr, "Error: Escape key handler returned false\n");
        return 1;
    }
    if (state.mode != VIM_MODE_NORMAL) {
        fprintf(stderr, "Error: expected mode VIM_MODE_NORMAL after Escape\n");
        return 1;
    }
    /* Cursor in normal mode should clamp to query_len - 1 = 13 (index of 'e') */
    if (state.cursor_pos != 13) {
        fprintf(stderr, "Error: expected cursor_pos 13, got %d\n", state.cursor_pos);
        return 1;
    }

    /* Test 'b': word back over "chrome" to index 8 ('c') */
    if (!vim_handle_key(&state, XKB_KEY_b, "b", 0) || state.cursor_pos != 8) {
        fprintf(stderr, "Error: 'b' motion failed, cursor_pos=%d (expected 8)\n", state.cursor_pos);
        return 1;
    }

    /* Test 'B': WORD back over "-9" to index 5 ('-') */
    if (!vim_handle_key(&state, XKB_KEY_B, "B", MOD_SHIFT) || state.cursor_pos != 5) {
        fprintf(stderr, "Error: 'B' motion failed, cursor_pos=%d (expected 5)\n", state.cursor_pos);
        return 1;
    }

    /* Test 'w': word forward from index 5 ('-') to index 6 ('9') */
    if (!vim_handle_key(&state, XKB_KEY_w, "w", 0) || state.cursor_pos != 6) {
        fprintf(stderr, "Error: 'w' motion failed, cursor_pos=%d (expected 6)\n", state.cursor_pos);
        return 1;
    }

    /* Test 'W': WORD forward from index 6 to index 8 ("chrome") */
    if (!vim_handle_key(&state, XKB_KEY_W, "W", MOD_SHIFT) || state.cursor_pos != 8) {
        fprintf(stderr, "Error: 'W' motion failed, cursor_pos=%d (expected 8)\n", state.cursor_pos);
        return 1;
    }

    /* Test 'b' again to go to index 5 ('-') */
    if (!vim_handle_key(&state, XKB_KEY_B, "B", MOD_SHIFT) || state.cursor_pos != 5) {
        fprintf(stderr, "Error: 'B' back to 5 failed, cursor_pos=%d\n", state.cursor_pos);
        return 1;
    }

    /* Test 'dw': delete word at '-'. 'dw' deletes "-" and stops at "9 chrome" (or deletes word token) */
    if (!vim_handle_key(&state, XKB_KEY_d, "d", 0)) {
        fprintf(stderr, "Error: 'd' operator failed\n");
        return 1;
    }
    if (!vim_handle_key(&state, XKB_KEY_w, "w", 0)) {
        fprintf(stderr, "Error: 'dw' motion failed\n");
        return 1;
    }

    /* Test 'u': undo 'dw', restoring "kill -9 chrome" */
    if (!vim_handle_key(&state, XKB_KEY_u, "u", 0)) {
        fprintf(stderr, "Error: 'u' undo failed\n");
        return 1;
    }
    if (strcmp(state.query, "kill -9 chrome") != 0) {
        fprintf(stderr, "Error: undo failed to restore query, got '%s'\n", state.query);
        return 1;
    }

    /* Test 'dd': delete entire line */
    if (!vim_handle_key(&state, XKB_KEY_d, "d", 0)) {
        fprintf(stderr, "Error: first 'd' of 'dd' failed\n");
        return 1;
    }
    if (!vim_handle_key(&state, XKB_KEY_d, "d", 0)) {
        fprintf(stderr, "Error: second 'd' of 'dd' failed\n");
        return 1;
    }
    if (state.query_len != 0 || state.query[0] != '\0') {
        fprintf(stderr, "Error: 'dd' failed, query='%s'\n", state.query);
        return 1;
    }

    /* Test scroll_row reset on typing */
    state.scroll_row = 4;
    state.selected_idx = 40;
    vim_handle_key(&state, XKB_KEY_i, "i", 0);
    vim_handle_key(&state, (xkb_keysym_t)'w', "w", 0);
    if (state.scroll_row != 0 || state.selected_idx != 0) {
        fprintf(stderr, "Error: expected scroll_row=0 and selected_idx=0 after typing 'w', got scroll_row=%d, selected_idx=%d\n",
                state.scroll_row, state.selected_idx);
        return 1;
    }

    /* Test fast failure reporting for shell commands */
    char err_buf[256] = {0};
    int rc = launch_raw_command("kill sometext", err_buf, sizeof(err_buf));
    if (rc == 0 || strstr(err_buf, "Err 1") == NULL || strstr(err_buf, "illegal pid: sometext") == NULL) {
        fprintf(stderr, "Error: launch_raw_command('kill sometext') failed, rc=%d, err_buf='%s'\n", rc, err_buf);
        return 1;
    }

    rc = launch_raw_command("pkill sometext", err_buf, sizeof(err_buf));
    if (rc == 0 || strstr(err_buf, "Err 1") == NULL) {
        fprintf(stderr, "Error: launch_raw_command('pkill sometext') failed, rc=%d, err_buf='%s'\n", rc, err_buf);
        return 1;
    }

    rc = launch_raw_command("true", err_buf, sizeof(err_buf));
    if (rc != 0) {
        fprintf(stderr, "Error: launch_raw_command('true') expected 0, got %d, err_buf='%s'\n", rc, err_buf);
        return 1;
    }

    /* Test error clearance on next keypress */
    strncpy(state.error_msg, "test error", sizeof(state.error_msg) - 1);
    vim_handle_key(&state, (xkb_keysym_t)'x', "x", 0);
    if (state.error_msg[0] != '\0') {
        fprintf(stderr, "Error: expected error_msg to clear on next keypress\n");
        return 1;
    }

    /* Test Command History & Focus Navigation */
    vim_init(&state);
    state.filtered_count = 5;
    state.layout.cols = 4;
    state.layout.visible_rows = 2;

    if (state.focus != FOCUS_TEXTBOX) {
        fprintf(stderr, "Error: expected initial focus == FOCUS_TEXTBOX\n");
        return 1;
    }

    /* Test Tab focuses grid */
    vim_handle_key(&state, XKB_KEY_Tab, NULL, 0);
    if (state.focus != FOCUS_GRID || state.selected_idx != 0) {
        fprintf(stderr, "Error: Tab failed to switch focus to FOCUS_GRID\n");
        return 1;
    }

    /* Test Shift-Tab at index 0 returns to FOCUS_TEXTBOX */
    vim_handle_key(&state, XKB_KEY_ISO_Left_Tab, NULL, 0);
    if (state.focus != FOCUS_TEXTBOX) {
        fprintf(stderr, "Error: Shift-Tab failed to return focus to FOCUS_TEXTBOX\n");
        return 1;
    }

    /* Test Up/Down history navigation */
    setenv("XDG_CACHE_HOME", "/tmp/launcher_test_cache", 1);
    state.history_count = 0;
    history_add(&state, "echo first");
    history_add(&state, "echo second");
    history_add(&state, "echo third");

    /* Type something in textbox */
    strncpy(state.query, "my typed cmd", sizeof(state.query) - 1);
    state.query_len = (int)strlen(state.query);
    state.cursor_pos = state.query_len;

    /* Up Arrow -> loads "echo third" */
    vim_handle_key(&state, XKB_KEY_Up, NULL, 0);
    if (strcmp(state.query, "echo third") != 0 || state.history_idx != 2) {
        fprintf(stderr, "Error: Up failed to load latest history item, got '%s'\n", state.query);
        return 1;
    }

    /* Ctrl-P -> loads "echo second" */
    vim_handle_key(&state, XKB_KEY_p, NULL, MOD_CTRL);
    if (strcmp(state.query, "echo second") != 0 || state.history_idx != 1) {
        fprintf(stderr, "Error: Ctrl-P failed to load previous history item, got '%s'\n", state.query);
        return 1;
    }

    /* Ctrl-N -> loads "echo third" */
    vim_handle_key(&state, XKB_KEY_n, NULL, MOD_CTRL);
    if (strcmp(state.query, "echo third") != 0 || state.history_idx != 2) {
        fprintf(stderr, "Error: Ctrl-N failed to load next history item, got '%s'\n", state.query);
        return 1;
    }

    /* Down Arrow -> restores original "my typed cmd" */
    vim_handle_key(&state, XKB_KEY_Down, NULL, 0);
    if (strcmp(state.query, "my typed cmd") != 0 || state.history_idx != -1) {
        fprintf(stderr, "Error: Down failed to restore typed query, got '%s'\n", state.query);
        return 1;
    }

    /* Down Arrow again when not in history -> focuses grid */
    state.filtered_count = 5;
    vim_handle_key(&state, XKB_KEY_Down, NULL, 0);
    if (state.focus != FOCUS_GRID || state.selected_idx != 0) {
        fprintf(stderr, "Error: Down when not in history failed to focus grid\n");
        return 1;
    }

    /* Up on row 0 returns focus to textbox */
    vim_handle_key(&state, XKB_KEY_Up, NULL, 0);
    if (state.focus != FOCUS_TEXTBOX) {
        fprintf(stderr, "Error: Up on row 0 failed to return to FOCUS_TEXTBOX\n");
        return 1;
    }

    unlink("/tmp/launcher_test_cache/launcher/history");
    rmdir("/tmp/launcher_test_cache/launcher");
    rmdir("/tmp/launcher_test_cache");
    unsetenv("XDG_CACHE_HOME");

    printf("VIM_TESTS_PASSED\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Signal handler
 * ------------------------------------------------------------------------- */
static void sigterm_handler(int sig) {
    (void)sig;
    app.state.running = false;
}

/* -------------------------------------------------------------------------
 * Wayland connection helper: auto-detect socket if WAYLAND_DISPLAY not set
 * ------------------------------------------------------------------------- */
static struct wl_display *connect_wayland_display(void) {
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display && wayland_display[0]) {
        return wl_display_connect(wayland_display);
    }

    /* Check default connect first */
    struct wl_display *d = wl_display_connect(NULL);
    if (d) return d;

    /* Check $XDG_RUNTIME_DIR/wayland-1 and wayland-0 */
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        char test_path[PATH_MAX];
        snprintf(test_path, sizeof(test_path), "%s/wayland-1", runtime_dir);
        if (access(test_path, F_OK) == 0) {
            d = wl_display_connect("wayland-1");
            if (d) return d;
        }
        snprintf(test_path, sizeof(test_path), "%s/wayland-0", runtime_dir);
        if (access(test_path, F_OK) == 0) {
            d = wl_display_connect("wayland-0");
            if (d) return d;
        }
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * main()
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    /* 1. CLI Options */
    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            printf("launcher %s\n", LAUNCHER_VERSION);
            return 0;
        }
        if (strcmp(argv[1], "--rebuild-cache") == 0) {
            LauncherState temp_state;
            memset(&temp_state, 0, sizeof(temp_state));
            int ret = cache_load_or_build(&temp_state, true);
            cache_free(&temp_state);
            return (ret == 0) ? 0 : 1;
        }
        if (strcmp(argv[1], "--dump-apps") == 0) {
            return do_dump_apps(argc, argv);
        }
        if (strcmp(argv[1], "--check-path") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Error: --check-path requires an argument\n");
                return 1;
            }
            return do_check_path(argv[2]);
        }
        if (strcmp(argv[1], "--render-test") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Error: --render-test requires an output filename\n");
                return 1;
            }
            const char *query = (argc >= 4) ? argv[3] : NULL;
            return do_render_test(argv[2], query);
        }
        if (strcmp(argv[1], "--test-vim") == 0) {
            return do_test_vim();
        }
    }

    /* 2. Single-Instance Lock */
    int lock_status = acquire_single_instance_lock(&app);
    if (lock_status > 0) {
        /* Sent SIGTERM to previous instance or toggled off */
        return 0;
    } else if (lock_status < 0) {
        fprintf(stderr, "Could not acquire single instance lock\n");
        return 1;
    }

    /* Setup signal handling for clean exit */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Initialize state */
    memset(&app.state, 0, sizeof(app.state));
    app.pointer_x = -1;
    app.pointer_y = -1;
    app.repeat_timer_fd = -1;
    app.repeat_rate = 25;
    app.repeat_delay_ms = 400;

    if (cache_load_or_build(&app.state, false) != 0) {
        fprintf(stderr, "Failed to load desktop application cache\n");
        release_single_instance_lock(&app);
        return 1;
    }

    filter_apps(&app.state);
    app.state.path_status = PATH_STATUS_EMPTY;

    if (ui_init(NULL, 20) != 0) {
        fprintf(stderr, "Failed to initialize UI renderer fonts\n");
        cache_free(&app.state);
        release_single_instance_lock(&app);
        return 1;
    }

    vim_init(&app.state);
    history_load(&app.state);

    /* Initialize XKB context */
    app.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!app.xkb_ctx) {
        fprintf(stderr, "Failed to create xkb context\n");
        ui_cleanup();
        cache_free(&app.state);
        release_single_instance_lock(&app);
        return 1;
    }

    /* Initialize repeat timer */
    app.repeat_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (app.repeat_timer_fd < 0) {
        perror("timerfd_create");
    }

    /* 3. Connect to Wayland */
    app.display = connect_wayland_display();
    if (!app.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        if (app.repeat_timer_fd >= 0) close(app.repeat_timer_fd);
        if (app.xkb_ctx) xkb_context_unref(app.xkb_ctx);
        ui_cleanup();
        cache_free(&app.state);
        release_single_instance_lock(&app);
        return 1;
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);

    /* First roundtrip to receive globals */
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.seat || !app.layer_shell) {
        fprintf(stderr, "Wayland compositor does not support required protocols (compositor, shm, seat, layer-shell)\n");
        app.state.running = false;
    } else {
        /* Second roundtrip to initialize seat and keyboard */
        wl_display_roundtrip(app.display);

        /* Create surface and layer surface */
        app.surface = wl_compositor_create_surface(app.compositor);
        app.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            app.layer_shell,
            app.surface,
            NULL,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            "launcher"
        );

        zwlr_layer_surface_v1_set_anchor(
            app.layer_surface,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
        );
        zwlr_layer_surface_v1_set_exclusive_zone(app.layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            app.layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
        );
        zwlr_layer_surface_v1_set_size(app.layer_surface, 0, 0);

        zwlr_layer_surface_v1_add_listener(app.layer_surface, &layer_surface_listener, &app);

        /* Initial commit without buffer to trigger configure */
        wl_surface_commit(app.surface);
        wl_display_flush(app.display);

        app.state.running = true;
    }

    /* 6. Event Loop with poll() */
    int wl_fd = wl_display_get_fd(app.display);

    while (app.state.running) {
        /* Dispatch pending display events */
        while (wl_display_prepare_read(app.display) != 0) {
            if (wl_display_dispatch_pending(app.display) < 0) {
                app.state.running = false;
                break;
            }
        }
        if (!app.state.running) break;

        wl_display_flush(app.display);

        struct pollfd pfd[2];
        pfd[0].fd = wl_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;

        pfd[1].fd = app.repeat_timer_fd;
        pfd[1].events = (app.repeat_timer_fd >= 0) ? POLLIN : 0;
        pfd[1].revents = 0;

        int poll_ret = poll(pfd, (app.repeat_timer_fd >= 0) ? 2 : 1, -1);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(app.display);
                continue;
            }
            wl_display_cancel_read(app.display);
            break;
        }

        if (pfd[0].revents & POLLIN) {
            if (wl_display_read_events(app.display) < 0) {
                app.state.running = false;
                break;
            }
        } else {
            wl_display_cancel_read(app.display);
        }

        if (pfd[0].revents & (POLLERR | POLLHUP)) {
            app.state.running = false;
            break;
        }

        /* Check repeat timer */
        if ((pfd[1].revents & POLLIN) && app.repeat_timer_fd >= 0) {
            uint64_t expirations = 0;
            ssize_t s = read(app.repeat_timer_fd, &expirations, sizeof(expirations));
            if (s == sizeof(expirations) && expirations > 0 && app.repeat_sym != XKB_KEY_NoSymbol) {
                for (uint64_t i = 0; i < expirations && app.state.running; i++) {
                    bool handled = vim_handle_key(&app.state, app.repeat_sym, app.repeat_utf8, app.current_mods);
                    if (handled) {
                        app.state.needs_redraw = true;
                    }
                }
            }
        }

        if (wl_display_dispatch_pending(app.display) < 0) {
            app.state.running = false;
            break;
        }

        if (app.state.needs_redraw) {
            render_and_commit(&app);
        }
    }

    /* Cleanup */
    disarm_repeat_timer(&app);
    if (app.repeat_timer_fd >= 0) {
        close(app.repeat_timer_fd);
        app.repeat_timer_fd = -1;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        destroy_shm_buffer(&app.buffers[i]);
    }

    if (app.layer_surface) {
        zwlr_layer_surface_v1_destroy(app.layer_surface);
        app.layer_surface = NULL;
    }
    if (app.surface) {
        wl_surface_destroy(app.surface);
        app.surface = NULL;
    }
    if (app.keyboard) {
        wl_keyboard_destroy(app.keyboard);
        app.keyboard = NULL;
    }
    if (app.pointer) {
        wl_pointer_destroy(app.pointer);
        app.pointer = NULL;
    }
    if (app.seat) {
        wl_seat_destroy(app.seat);
        app.seat = NULL;
    }
    if (app.shm) {
        wl_shm_destroy(app.shm);
        app.shm = NULL;
    }
    if (app.compositor) {
        wl_compositor_destroy(app.compositor);
        app.compositor = NULL;
    }
    if (app.layer_shell) {
        zwlr_layer_shell_v1_destroy(app.layer_shell);
        app.layer_shell = NULL;
    }
    if (app.registry) {
        wl_registry_destroy(app.registry);
        app.registry = NULL;
    }
    if (app.display) {
        wl_display_disconnect(app.display);
        app.display = NULL;
    }

    if (app.xkb_state) {
        xkb_state_unref(app.xkb_state);
        app.xkb_state = NULL;
    }
    if (app.xkb_keymap) {
        xkb_keymap_unref(app.xkb_keymap);
        app.xkb_keymap = NULL;
    }
    if (app.xkb_ctx) {
        xkb_context_unref(app.xkb_ctx);
        app.xkb_ctx = NULL;
    }

    ui_cleanup();
    cache_free(&app.state);
    release_single_instance_lock(&app);

    return 0;
}
