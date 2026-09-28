#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#ifndef LAUNCHER_VERSION
#define LAUNCHER_VERSION "0.1.1"
#endif

#define ICON_SIZE 64
#define MAX_APPS 512
#define MAX_QUERY 256

typedef enum {
    PATH_STATUS_INVALID = -1,
    PATH_STATUS_EMPTY = 0,
    PATH_STATUS_VALID = 1
} PathStatus;

typedef enum {
    VIM_MODE_INSERT = 0,
    VIM_MODE_NORMAL = 1
} VimMode;

typedef enum {
    FOCUS_GRID = 0,
    FOCUS_TEXTBOX = 1
} FocusTarget;

typedef struct {
    char desktop_id[128];
    char name[128];
    char generic_name[128];
    char keywords[256];
    char exec_cmd[512];
    char icon_name[128];
    bool terminal;
    bool has_icon;
    uint32_t launch_count;
    uint32_t icon_pixels[ICON_SIZE * ICON_SIZE]; // ARGB8888 premultiplied
} AppEntry;

typedef struct {
    int textbox_x;
    int textbox_y;
    int textbox_w;
    int textbox_h;
    int grid_x;
    int grid_y;
    int cols;
    int visible_rows;
    int cell_w;
    int cell_h;
    int cell_pad_x;
    int cell_pad_y;
} LayoutMetrics;

typedef struct {
    AppEntry *apps;
    size_t app_count;
    int filtered[MAX_APPS];
    size_t filtered_count;

    char query[MAX_QUERY];
    int query_len;
    int cursor_pos;

    char undo_query[MAX_QUERY];
    int undo_cursor;
    char yank_buf[MAX_QUERY];

    VimMode mode;
    char pending_op; // 0, 'd', 'c', 'g'
    FocusTarget focus;

    int selected_idx; // index in filtered[] (0 .. filtered_count-1)
    int scroll_row;

    PathStatus path_status;
    LayoutMetrics layout;
    uint32_t screen_w;
    uint32_t screen_h;

    bool running;
    bool needs_redraw;
    char error_msg[256];
} LauncherState;

/* desktop_cache.c */
int cache_load_or_build(LauncherState *state, bool force_rebuild);
void cache_free(LauncherState *state);
void cache_record_launch(LauncherState *state, int app_index);
void filter_apps(LauncherState *state);
PathStatus check_path_executable(const char *query);
int launch_desktop_app(const AppEntry *app);
int launch_raw_command(const char *cmd, char *err_out, size_t err_out_size);

/* vim_input.c */
void vim_init(LauncherState *state);
bool vim_handle_key(LauncherState *state, xkb_keysym_t sym, const char *utf8, uint32_t mods);

/* ui_render.c */
int ui_init(const char *preferred_font, int font_size_px);
void ui_cleanup(void);
void ui_compute_layout(LauncherState *state, uint32_t width, uint32_t height);
void ui_render_frame(LauncherState *state, uint32_t *argb_buf, uint32_t width, uint32_t height);
int ui_grid_index_at(const LauncherState *state, int x, int y);
bool ui_is_in_textbox(const LauncherState *state, int x, int y);

/* Modifier bitmasks passed to vim_handle_key */
#define MOD_SHIFT (1u << 0)
#define MOD_CTRL  (1u << 1)
#define MOD_ALT   (1u << 2)

#endif /* LAUNCHER_H */
