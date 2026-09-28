#define _GNU_SOURCE
#include "launcher.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <png.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#define CACHE_MAGIC 0x4C4E4348 /* "LNCH" */
#define CACHE_VERSION 2

#define PATH_BUF_SIZE (PATH_MAX * 2)

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t dir_mtime_hash;
    uint32_t app_count;
} CacheHeader;
#pragma pack(pop)

/* Helper: trim leading/trailing whitespace in place */
static char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/* Helper: check if file exists */
static bool file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* Helper: check if directory exists */
static bool dir_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Helper: find case-insensitive substring */
static const char *strcasestr_custom(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (*needle == '\0') return haystack;
    size_t needle_len = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
    }
    return NULL;
}

/* --- Cache Directory & Paths --- */

static void get_cache_path(char *out, size_t maxlen) {
    char dir[PATH_BUF_SIZE];
    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    if (xdg_cache && xdg_cache[0] != '\0') {
        snprintf(dir, sizeof(dir), "%s/launcher", xdg_cache);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(dir, sizeof(dir), "%s/.cache/launcher", home);
    }
    mkdir(dir, 0755);
    snprintf(out, maxlen, "%s/cache.bin", dir);
}

/* Get list of desktop directories to scan */
#define MAX_SEARCH_DIRS 64
static int get_desktop_dirs(char dirs[MAX_SEARCH_DIRS][PATH_BUF_SIZE]) {
    int count = 0;
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    /* 1. $XDG_DATA_HOME/applications or ~/.local/share/applications */
    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0]) {
        snprintf(dirs[count++], PATH_BUF_SIZE, "%s/applications", xdg_data);
    } else {
        snprintf(dirs[count++], PATH_BUF_SIZE, "%s/.local/share/applications", home);
    }

    /* 2. ~/.local/share/flatpak/exports/share/applications */
    snprintf(dirs[count++], PATH_BUF_SIZE, "%s/.local/share/flatpak/exports/share/applications", home);

    /* 3. $XDG_DATA_DIRS + /applications */
    const char *xdg_dirs = getenv("XDG_DATA_DIRS");
    if (xdg_dirs && xdg_dirs[0]) {
        char buf[4096];
        strncpy(buf, xdg_dirs, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *saveptr = NULL;
        char *token = strtok_r(buf, ":", &saveptr);
        while (token && count < MAX_SEARCH_DIRS - 4) {
            if (token[0]) {
                snprintf(dirs[count++], PATH_BUF_SIZE, "%s/applications", token);
            }
            token = strtok_r(NULL, ":", &saveptr);
        }
    } else {
        snprintf(dirs[count++], PATH_BUF_SIZE, "/usr/local/share/applications");
        snprintf(dirs[count++], PATH_BUF_SIZE, "/usr/share/applications");
    }

    /* 4. /var/lib/flatpak/exports/share/applications */
    if (count < MAX_SEARCH_DIRS) {
        snprintf(dirs[count++], PATH_BUF_SIZE, "/var/lib/flatpak/exports/share/applications");
    }

    /* 5. /var/lib/snapd/desktop/applications */
    if (count < MAX_SEARCH_DIRS) {
        snprintf(dirs[count++], PATH_BUF_SIZE, "/var/lib/snapd/desktop/applications");
    }

    return count;
}

/* Compute hash of directory modification times */
static uint64_t compute_dir_mtime_hash(void) {
    char dirs[MAX_SEARCH_DIRS][PATH_BUF_SIZE];
    int dir_count = get_desktop_dirs(dirs);
    uint64_t hash = 14695981039346656037ULL; /* FNV-1a basis */

    for (int i = 0; i < dir_count; i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            uint64_t mtime = (uint64_t)st.st_mtime;
            hash ^= mtime;
            hash *= 1099511628211ULL;

            /* Check 1 level deep subdirectories */
            DIR *d = opendir(dirs[i]);
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_name[0] == '.') continue;
                    char sub[PATH_BUF_SIZE];
                    snprintf(sub, sizeof(sub), "%s/%s", dirs[i], ent->d_name);
                    struct stat sub_st;
                    if (stat(sub, &sub_st) == 0 && S_ISDIR(sub_st.st_mode)) {
                        uint64_t sub_mtime = (uint64_t)sub_st.st_mtime;
                        hash ^= sub_mtime;
                        hash *= 1099511628211ULL;
                    }
                }
                closedir(d);
            }
        }
    }
    return hash;
}

/* --- Exec field code stripping --- */

static void clean_exec_cmd(const char *src, char *dst, size_t dst_max) {
    size_t d = 0;
    size_t s = 0;
    size_t len = strlen(src);

    while (s < len && d + 1 < dst_max) {
        if (src[s] == '%') {
            if (s + 1 < len) {
                char code = src[s + 1];
                if (code == '%') {
                    dst[d++] = '%';
                    s += 2;
                    continue;
                } else if (code == 'f' || code == 'F' || code == 'u' || code == 'U' ||
                           code == 'd' || code == 'D' || code == 'n' || code == 'N' ||
                           code == 'i' || code == 'c' || code == 'k' || code == 'v' ||
                           code == 'm') {
                    s += 2;
                    continue;
                }
            }
        }
        dst[d++] = src[s++];
    }
    dst[d] = '\0';

    /* Collapse repeated spaces and trim */
    char *trimmed = trim_whitespace(dst);
    if (trimmed != dst) {
        memmove(dst, trimmed, strlen(trimmed) + 1);
    }
    /* Collapse multiple internal spaces */
    size_t w = 0;
    bool in_space = false;
    for (size_t r = 0; dst[r] != '\0'; r++) {
        if (isspace((unsigned char)dst[r])) {
            if (!in_space) {
                dst[w++] = ' ';
                in_space = true;
            }
        } else {
            dst[w++] = dst[r];
            in_space = false;
        }
    }
    dst[w] = '\0';
}

/* --- Icon Resolution & Rasterization --- */

static FT_Library s_ft_lib = NULL;
static FT_Face s_ft_face = NULL;
static bool s_ft_attempted = false;
static NSVGrasterizer *s_rast = NULL;

static void init_fallback_font(void) {
    if (s_ft_attempted) return;
    s_ft_attempted = true;

    if (FT_Init_FreeType(&s_ft_lib) != 0) {
        s_ft_lib = NULL;
        return;
    }

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char path_fira_bold[PATH_BUF_SIZE];
    char path_fira_reg[PATH_BUF_SIZE];
    snprintf(path_fira_bold, sizeof(path_fira_bold), "%s/.fonts/fira/FiraMono-Bold.ttf", home);
    snprintf(path_fira_reg, sizeof(path_fira_reg), "%s/.fonts/fira/FiraMono-Regular.ttf", home);

    const char *font_candidates[] = {
        path_fira_bold,
        path_fira_reg,
        "/usr/share/fonts/truetype/fira/FiraMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    };

    for (size_t i = 0; i < sizeof(font_candidates) / sizeof(font_candidates[0]); i++) {
        if (file_exists(font_candidates[i])) {
            if (FT_New_Face(s_ft_lib, font_candidates[i], 0, &s_ft_face) == 0) {
                FT_Set_Pixel_Sizes(s_ft_face, 0, 36);
                return;
            }
        }
    }
}

static void cleanup_icon_resources(void) {
    if (s_ft_face) {
        FT_Done_Face(s_ft_face);
        s_ft_face = NULL;
    }
    if (s_ft_lib) {
        FT_Done_FreeType(s_ft_lib);
        s_ft_lib = NULL;
    }
    s_ft_attempted = false;

    if (s_rast) {
        nsvgDeleteRasterizer(s_rast);
        s_rast = NULL;
    }
}

static void generate_fallback_icon(AppEntry *app) {
    uint32_t hash = 5381;
    for (const char *c = app->name; *c; c++) {
        hash = ((hash << 5) + hash) + (unsigned char)*c;
    }

    /* Pleasant palette based on hash */
    static const uint32_t colors[] = {
        0xFF3498DB, /* Blue */
        0xFF2ECC71, /* Green */
        0xFFE74C3C, /* Red */
        0xFF9B59B6, /* Purple */
        0xFFF39C12, /* Orange */
        0xFF1ABC9C, /* Turquoise */
        0xFFE67E22, /* Carrot */
        0xFF34495E, /* Dark blue */
        0xFF16A085, /* Green sea */
        0xFFD35400, /* Pumpkin */
    };
    uint32_t bg_color = colors[hash % (sizeof(colors) / sizeof(colors[0]))];
    uint8_t bg_a = (bg_color >> 24) & 0xFF;
    uint8_t bg_r = (bg_color >> 16) & 0xFF;
    uint8_t bg_g = (bg_color >> 8) & 0xFF;
    uint8_t bg_b = bg_color & 0xFF;
    uint32_t bg_pre = (bg_a << 24) |
                      (((uint32_t)bg_r * bg_a / 255) << 16) |
                      (((uint32_t)bg_g * bg_a / 255) << 8) |
                      ((uint32_t)bg_b * bg_a / 255);

    /* Fill rounded square with radius 12 in 64x64 */
    const float radius = 12.0f;
    const float min_c = 4.0f;
    const float max_c = 59.0f;

    for (int y = 0; y < ICON_SIZE; y++) {
        for (int x = 0; x < ICON_SIZE; x++) {
            float fx = (float)x;
            float fy = (float)y;
            float dx = 0.0f;
            float dy = 0.0f;

            if (fx < min_c + radius) dx = (min_c + radius) - fx;
            else if (fx > max_c - radius) dx = fx - (max_c - radius);

            if (fy < min_c + radius) dy = (min_c + radius) - fy;
            else if (fy > max_c - radius) dy = fy - (max_c - radius);

            if (fx >= min_c && fx <= max_c && fy >= min_c && fy <= max_c) {
                if (dx > 0 && dy > 0) {
                    float dist = sqrtf(dx * dx + dy * dy);
                    if (dist <= radius) {
                        app->icon_pixels[y * ICON_SIZE + x] = bg_pre;
                    } else if (dist <= radius + 1.0f) {
                        float alpha_factor = (radius + 1.0f - dist);
                        uint8_t a = (uint8_t)(bg_a * alpha_factor);
                        app->icon_pixels[y * ICON_SIZE + x] =
                            (a << 24) |
                            (((uint32_t)bg_r * a / 255) << 16) |
                            (((uint32_t)bg_g * a / 255) << 8) |
                            ((uint32_t)bg_b * a / 255);
                    } else {
                        app->icon_pixels[y * ICON_SIZE + x] = 0;
                    }
                } else {
                    app->icon_pixels[y * ICON_SIZE + x] = bg_pre;
                }
            } else {
                app->icon_pixels[y * ICON_SIZE + x] = 0;
            }
        }
    }

    /* Render uppercase initial character centered using FreeType + Fira Mono */
    char letter = toupper((unsigned char)app->name[0]);
    if (letter < ' ' || letter > '~') letter = '?';

    init_fallback_font();

    if (s_ft_face) {
        if (FT_Load_Char(s_ft_face, (FT_ULong)letter, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot slot = s_ft_face->glyph;
            int g_w = (int)slot->bitmap.width;
            int g_h = (int)slot->bitmap.rows;
            int g_top = slot->bitmap_top;
            int g_left = slot->bitmap_left;

            int dst_x = (ICON_SIZE - g_w) / 2;
            int baseline = 46; /* Optical center for 36px font */
            int dst_y = baseline - g_top;
            (void)g_left;

            for (int r = 0; r < g_h; r++) {
                int py = dst_y + r;
                if (py < 0 || py >= ICON_SIZE) continue;

                for (int c = 0; c < g_w; c++) {
                    int px = dst_x + c;
                    if (px < 0 || px >= ICON_SIZE) continue;

                    uint8_t alpha = slot->bitmap.buffer[r * slot->bitmap.pitch + c];
                    if (alpha == 0) continue;

                    int idx = py * ICON_SIZE + px;
                    uint32_t cur = app->icon_pixels[idx];
                    uint8_t cur_a = (cur >> 24) & 0xFF;
                    uint8_t cur_r = (cur >> 16) & 0xFF;
                    uint8_t cur_g = (cur >> 8) & 0xFF;
                    uint8_t cur_b = cur & 0xFF;

                    /* Blend 0xFFFFFFFF text (premultiplied: a, a, a, a) over cur (premultiplied) */
                    /* out = src + dst * (1 - src_a) */
                    uint32_t inv_a = 255 - alpha;
                    uint8_t out_a = (uint8_t)(alpha + (cur_a * inv_a + 127) / 255);
                    uint8_t out_r = (uint8_t)(alpha + (cur_r * inv_a + 127) / 255);
                    uint8_t out_g = (uint8_t)(alpha + (cur_g * inv_a + 127) / 255);
                    uint8_t out_b = (uint8_t)(alpha + (cur_b * inv_a + 127) / 255);

                    app->icon_pixels[idx] = ((uint32_t)out_a << 24) |
                                            ((uint32_t)out_r << 16) |
                                            ((uint32_t)out_g << 8) |
                                            (uint32_t)out_b;
                }
            }
        }
    }

    app->has_icon = true;
}

static bool rasterize_svg(const char *path, uint32_t *out_pixels) {
    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) return false;

    if (!image->shapes) {
        nsvgDelete(image);
        return false;
    }

    if (!s_rast) {
        s_rast = nsvgCreateRasterizer();
        if (!s_rast) {
            nsvgDelete(image);
            return false;
        }
    }

    float img_w = image->width > 0.0f ? image->width : 64.0f;
    float img_h = image->height > 0.0f ? image->height : 64.0f;
    float max_dim = img_w > img_h ? img_w : img_h;
    if (max_dim <= 0.0f) max_dim = 64.0f;

    float scale = 60.0f / max_dim; /* leave 2px padding */
    float target_w = img_w * scale;
    float target_h = img_h * scale;
    float tx = (64.0f - target_w) / 2.0f;
    float ty = (64.0f - target_h) / 2.0f;

    static uint8_t rgba[ICON_SIZE * ICON_SIZE * 4];
    memset(rgba, 0, sizeof(rgba));

    nsvgRasterize(s_rast, image, tx, ty, scale, rgba, ICON_SIZE, ICON_SIZE, ICON_SIZE * 4);

    bool has_visible_pixel = false;
    for (int i = 0; i < ICON_SIZE * ICON_SIZE; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];

        if (a > 16) {
            has_visible_pixel = true;
        }

        uint8_t r_pre = (uint8_t)(((uint32_t)r * a) / 255);
        uint8_t g_pre = (uint8_t)(((uint32_t)g * a) / 255);
        uint8_t b_pre = (uint8_t)(((uint32_t)b * a) / 255);

        out_pixels[i] = ((uint32_t)a << 24) |
                        ((uint32_t)r_pre << 16) |
                        ((uint32_t)g_pre << 8) |
                        (uint32_t)b_pre;
    }

    nsvgDelete(image);

    if (!has_visible_pixel) {
        return false;
    }

    return true;
}

static bool rasterize_png(const char *path, uint32_t *out_pixels) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    png_byte header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8) != 0) {
        fclose(fp);
        return false;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    png_uint_32 width = png_get_image_width(png, info);
    png_uint_32 height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    /* Normalize to 8-bit RGBA */
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    png_read_update_info(png, info);

    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    if (!row_pointers) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }

    size_t rowbytes = png_get_rowbytes(png, info);
    uint8_t *raw_buf = (uint8_t *)malloc(rowbytes * height);
    if (!raw_buf) {
        free(row_pointers);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }

    for (png_uint_32 y = 0; y < height; y++) {
        row_pointers[y] = raw_buf + y * rowbytes;
    }

    png_read_image(png, row_pointers);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    /* Scale to 64x64 using bilinear filtering */
    for (int dst_y = 0; dst_y < ICON_SIZE; dst_y++) {
        float src_yf = ((float)dst_y + 0.5f) * ((float)height / (float)ICON_SIZE) - 0.5f;
        int y0 = (int)floorf(src_yf);
        int y1 = y0 + 1;
        float wy = src_yf - (float)y0;
        if (y0 < 0) y0 = 0;
        if (y1 >= (int)height) y1 = height - 1;

        for (int dst_x = 0; dst_x < ICON_SIZE; dst_x++) {
            float src_xf = ((float)dst_x + 0.5f) * ((float)width / (float)ICON_SIZE) - 0.5f;
            int x0 = (int)floorf(src_xf);
            int x1 = x0 + 1;
            float wx = src_xf - (float)x0;
            if (x0 < 0) x0 = 0;
            if (x1 >= (int)width) x1 = width - 1;

            uint8_t *p00 = row_pointers[y0] + x0 * 4;
            uint8_t *p10 = row_pointers[y0] + x1 * 4;
            uint8_t *p01 = row_pointers[y1] + x0 * 4;
            uint8_t *p11 = row_pointers[y1] + x1 * 4;

            float w00 = (1.0f - wx) * (1.0f - wy);
            float w10 = wx * (1.0f - wy);
            float w01 = (1.0f - wx) * wy;
            float w11 = wx * wy;

            float rf = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;
            float gf = p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11;
            float bf = p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11;
            float af = p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11;

            uint8_t a = (uint8_t)(af + 0.5f);
            uint8_t r = (uint8_t)(rf + 0.5f);
            uint8_t g = (uint8_t)(gf + 0.5f);
            uint8_t b = (uint8_t)(bf + 0.5f);

            uint8_t r_pre = (uint8_t)(((uint32_t)r * a) / 255);
            uint8_t g_pre = (uint8_t)(((uint32_t)g * a) / 255);
            uint8_t b_pre = (uint8_t)(((uint32_t)b * a) / 255);

            out_pixels[dst_y * ICON_SIZE + dst_x] =
                ((uint32_t)a << 24) |
                ((uint32_t)r_pre << 16) |
                ((uint32_t)g_pre << 8) |
                (uint32_t)b_pre;
        }
    }

    free(raw_buf);
    free(row_pointers);
    return true;
}

static bool try_load_icon_file(const char *path, uint32_t *pixels) {
    size_t len = strlen(path);
    if (len > 4 && strcasecmp(path + len - 4, ".svg") == 0) {
        return rasterize_svg(path, pixels);
    }
    if (len > 4 && strcasecmp(path + len - 4, ".png") == 0) {
        return rasterize_png(path, pixels);
    }
    /* Try svg then png if unknown */
    if (rasterize_svg(path, pixels)) return true;
    if (rasterize_png(path, pixels)) return true;
    return false;
}

static void resolve_and_render_icon(AppEntry *app) {
    memset(app->icon_pixels, 0, sizeof(app->icon_pixels));
    app->has_icon = false;

    if (app->icon_name[0] == '\0') {
        generate_fallback_icon(app);
        return;
    }

    /* Absolute path */
    if (app->icon_name[0] == '/') {
        if (try_load_icon_file(app->icon_name, app->icon_pixels)) {
            app->has_icon = true;
            return;
        }
        /* Check with .png / .svg appended if no extension */
        char tmp[PATH_BUF_SIZE];
        snprintf(tmp, sizeof(tmp), "%s.png", app->icon_name);
        if (try_load_icon_file(tmp, app->icon_pixels)) {
            app->has_icon = true;
            return;
        }
        snprintf(tmp, sizeof(tmp), "%s.svg", app->icon_name);
        if (try_load_icon_file(tmp, app->icon_pixels)) {
            app->has_icon = true;
            return;
        }
        generate_fallback_icon(app);
        return;
    }

    /* Search icon themes */
    static const char *themes[] = {
        "Papirus",
        "Papirus-Dark",
        "hicolor",
        "Adwaita"
    };

    static const char *subdirs[] = {
        "64x64/apps",
        "64x64/devices",
        "64x64/actions",
        "64x64/status",
        "64x64/categories",
        "64x64/places",
        "48x48/apps",
        "48x48/devices",
        "48x48/actions",
        "48x48/status",
        "128x128/apps",
        "128x128/devices",
        "scalable/apps",
        "scalable/devices",
        "scalable/actions",
        "scalable/status",
        "32x32/apps",
        "16x16@2x/actions",
        "16x16@2x/devices",
        "16x16@2x/panel",
        "16x16@2x/apps",
        "256x256/apps",
        "apps/64",
        "apps/48",
        "apps/scalable"
    };

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char base_dirs[4][PATH_BUF_SIZE];
    int base_count = 0;
    snprintf(base_dirs[base_count++], PATH_BUF_SIZE, "%s/.local/share/icons", home);
    snprintf(base_dirs[base_count++], PATH_BUF_SIZE, "/usr/local/share/icons");
    snprintf(base_dirs[base_count++], PATH_BUF_SIZE, "/usr/share/icons");
    snprintf(base_dirs[base_count++], PATH_BUF_SIZE, "/usr/share/pixmaps");

    /* Strip extension from icon_name if present */
    char clean_name[128];
    strncpy(clean_name, app->icon_name, sizeof(clean_name) - 1);
    clean_name[sizeof(clean_name) - 1] = '\0';
    size_t cn_len = strlen(clean_name);
    if (cn_len > 4 && (strcasecmp(clean_name + cn_len - 4, ".png") == 0 ||
                       strcasecmp(clean_name + cn_len - 4, ".svg") == 0)) {
        clean_name[cn_len - 4] = '\0';
        cn_len -= 4;
    }

    /* Candidate icon names to search */
    char candidates[4][128];
    int cand_count = 0;
    strncpy(candidates[cand_count++], clean_name, sizeof(candidates[0]) - 1);
    candidates[cand_count - 1][sizeof(candidates[0]) - 1] = '\0';

    /* If clean_name ends with "-symbolic", try stripped version without "-symbolic" */
    if (cn_len > 9 && strcmp(clean_name + cn_len - 9, "-symbolic") == 0) {
        char no_sym[128];
        strncpy(no_sym, clean_name, cn_len - 9);
        no_sym[cn_len - 9] = '\0';
        if (no_sym[0] && cand_count < 4) {
            strncpy(candidates[cand_count++], no_sym, sizeof(candidates[0]) - 1);
            candidates[cand_count - 1][sizeof(candidates[0]) - 1] = '\0';
        }
    }

    /* Special case: Chrome Remote Desktop fallback */
    if (strcasecmp(app->name, "Chrome Remote Desktop") == 0 && cand_count < 4) {
        strncpy(candidates[cand_count++], "chrome-remote-desktop", sizeof(candidates[0]) - 1);
        candidates[cand_count - 1][sizeof(candidates[0]) - 1] = '\0';
    }

    char test_path[PATH_BUF_SIZE];

    /* 1. Theme search */
    for (int c = 0; c < cand_count; c++) {
        const char *cname = candidates[c];
        for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
            for (int b = 0; b < base_count; b++) {
                for (size_t s = 0; s < sizeof(subdirs) / sizeof(subdirs[0]); s++) {
                    /* Try .svg */
                    snprintf(test_path, sizeof(test_path), "%s/%s/%s/%s.svg",
                             base_dirs[b], themes[t], subdirs[s], cname);
                    if (file_exists(test_path) && try_load_icon_file(test_path, app->icon_pixels)) {
                        app->has_icon = true;
                        return;
                    }
                    /* Try .png */
                    snprintf(test_path, sizeof(test_path), "%s/%s/%s/%s.png",
                             base_dirs[b], themes[t], subdirs[s], cname);
                    if (file_exists(test_path) && try_load_icon_file(test_path, app->icon_pixels)) {
                        app->has_icon = true;
                        return;
                    }
                }
            }
        }
    }

    /* 2. Direct pixmaps / icons search */
    for (int c = 0; c < cand_count; c++) {
        const char *cname = candidates[c];
        for (int b = 0; b < base_count; b++) {
            snprintf(test_path, sizeof(test_path), "%s/%s.svg", base_dirs[b], cname);
            if (file_exists(test_path) && try_load_icon_file(test_path, app->icon_pixels)) {
                app->has_icon = true;
                return;
            }
            snprintf(test_path, sizeof(test_path), "%s/%s.png", base_dirs[b], cname);
            if (file_exists(test_path) && try_load_icon_file(test_path, app->icon_pixels)) {
                app->has_icon = true;
                return;
            }
            snprintf(test_path, sizeof(test_path), "%s/%s", base_dirs[b], cname);
            if (file_exists(test_path) && try_load_icon_file(test_path, app->icon_pixels)) {
                app->has_icon = true;
                return;
            }
        }
    }

    generate_fallback_icon(app);
}

/* --- .desktop File Parsing --- */

static bool parse_desktop_file(const char *filepath, const char *desktop_id, AppEntry *app) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return false;

    memset(app, 0, sizeof(*app));
    strncpy(app->desktop_id, desktop_id, sizeof(app->desktop_id) - 1);

    char line[1024];
    bool in_desktop_entry = false;
    bool is_app = false;
    bool no_display = false;
    bool hidden = false;
    bool kde_only = false;
    bool tryexec_failed = false;

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim_whitespace(line);
        if (*p == '#' || *p == '\0') continue;

        if (*p == '[') {
            if (strcmp(p, "[Desktop Entry]") == 0) {
                in_desktop_entry = true;
                continue;
            } else {
                if (in_desktop_entry) {
                    /* Next section started, stop parsing */
                    break;
                }
                continue;
            }
        }

        if (!in_desktop_entry) continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_whitespace(p);
        char *val = trim_whitespace(eq + 1);

        /* Only parse unlocalized keys */
        if (strchr(key, '[')) continue;

        if (strcmp(key, "Type") == 0) {
            if (strcmp(val, "Application") == 0) is_app = true;
        } else if (strcmp(key, "NoDisplay") == 0) {
            if (strcasecmp(val, "true") == 0) no_display = true;
        } else if (strcmp(key, "Hidden") == 0) {
            if (strcasecmp(val, "true") == 0) hidden = true;
        } else if (strcmp(key, "OnlyShowIn") == 0) {
            /* If contains KDE and does not contain sway, wlroots, or GNOME, reject */
            if (strcasestr_custom(val, "KDE") != NULL &&
                strcasestr_custom(val, "sway") == NULL &&
                strcasestr_custom(val, "wlroots") == NULL &&
                strcasestr_custom(val, "GNOME") == NULL) {
                kde_only = true;
            }
        } else if (strcmp(key, "TryExec") == 0) {
            if (val[0] && check_path_executable(val) != PATH_STATUS_VALID) {
                tryexec_failed = true;
            }
        } else if (strcmp(key, "Name") == 0) {
            strncpy(app->name, val, sizeof(app->name) - 1);
        } else if (strcmp(key, "GenericName") == 0) {
            strncpy(app->generic_name, val, sizeof(app->generic_name) - 1);
        } else if (strcmp(key, "Keywords") == 0) {
            strncpy(app->keywords, val, sizeof(app->keywords) - 1);
        } else if (strcmp(key, "Exec") == 0) {
            clean_exec_cmd(val, app->exec_cmd, sizeof(app->exec_cmd));
        } else if (strcmp(key, "Icon") == 0) {
            strncpy(app->icon_name, val, sizeof(app->icon_name) - 1);
        } else if (strcmp(key, "Terminal") == 0) {
            if (strcasecmp(val, "true") == 0) app->terminal = true;
        }
    }

    fclose(fp);

    if (!is_app || no_display || hidden || kde_only || tryexec_failed ||
        app->name[0] == '\0' || app->exec_cmd[0] == '\0') {
        return false;
    }

    /* Skip foot-server.desktop and footclient.desktop (or background server daemon) */
    if (strcasecmp(desktop_id, "foot-server.desktop") == 0 ||
        strcasecmp(desktop_id, "footclient.desktop") == 0 ||
        strcasecmp(app->exec_cmd, "foot --server") == 0 ||
        strcasecmp(app->exec_cmd, "footclient") == 0) {
        return false;
    }

    return true;
}

/* Compare apps for initial alphabetical sort by name */
static int compare_apps_alphabetical(const void *a, const void *b) {
    const AppEntry *ea = (const AppEntry *)a;
    const AppEntry *eb = (const AppEntry *)b;
    int cmp = strcasecmp(ea->name, eb->name);
    if (cmp != 0) return cmp;
    return strcmp(ea->desktop_id, eb->desktop_id);
}

typedef struct {
    char name[256];
    bool is_dir;
} DirItem;

static int compare_dir_items(const void *a, const void *b) {
    const DirItem *ia = (const DirItem *)a;
    const DirItem *ib = (const DirItem *)b;
    /* Shorter desktop name first (e.g. nvim.desktop before org.neovim.nvim.desktop) */
    size_t la = strlen(ia->name);
    size_t lb = strlen(ib->name);
    if (la != lb) return (la < lb) ? -1 : 1;
    return strcmp(ia->name, ib->name);
}

/* Scan all .desktop files across all search directories */
static size_t scan_all_desktop_apps(AppEntry *apps, size_t max_apps, const AppEntry *old_entries, size_t old_count) {
    char dirs[MAX_SEARCH_DIRS][PATH_BUF_SIZE];
    int dir_count = get_desktop_dirs(dirs);
    size_t count = 0;

    for (int d = 0; d < dir_count; d++) {
        if (!dir_exists(dirs[d])) continue;

        /* Scan top level and 1 level deep */
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;

        DirItem top_items[1024];
        size_t top_count = 0;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && top_count < 1024) {
            if (ent->d_name[0] == '.') continue;
            strncpy(top_items[top_count].name, ent->d_name, sizeof(top_items[top_count].name) - 1);
            top_items[top_count].name[sizeof(top_items[top_count].name) - 1] = '\0';
            char fullpath[PATH_BUF_SIZE];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dirs[d], ent->d_name);
            struct stat st;
            if (stat(fullpath, &st) == 0) {
                top_items[top_count].is_dir = S_ISDIR(st.st_mode);
                top_count++;
            }
        }
        closedir(dir);

        qsort(top_items, top_count, sizeof(DirItem), compare_dir_items);

        for (size_t t = 0; t < top_count; t++) {
            char fullpath[PATH_BUF_SIZE];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dirs[d], top_items[t].name);

            if (top_items[t].is_dir) {
                /* 1 level deep subdirectory */
                DIR *subdir = opendir(fullpath);
                if (subdir) {
                    DirItem sub_items[1024];
                    size_t sub_count = 0;
                    struct dirent *subent;
                    while ((subent = readdir(subdir)) != NULL && sub_count < 1024) {
                        if (subent->d_name[0] == '.') continue;
                        size_t subname_len = strlen(subent->d_name);
                        if (subname_len > 8 && strcmp(subent->d_name + subname_len - 8, ".desktop") == 0) {
                            strncpy(sub_items[sub_count].name, subent->d_name, sizeof(sub_items[sub_count].name) - 1);
                            sub_items[sub_count].name[sizeof(sub_items[sub_count].name) - 1] = '\0';
                            sub_items[sub_count].is_dir = false;
                            sub_count++;
                        }
                    }
                    closedir(subdir);

                    qsort(sub_items, sub_count, sizeof(DirItem), compare_dir_items);

                    for (size_t s = 0; s < sub_count; s++) {
                        /* Deduplicate by desktop_id */
                        bool dup = false;
                        for (size_t i = 0; i < count; i++) {
                            if (strcmp(apps[i].desktop_id, sub_items[s].name) == 0) {
                                dup = true;
                                break;
                            }
                        }
                        if (dup) continue;

                        char subpath[PATH_BUF_SIZE];
                        snprintf(subpath, sizeof(subpath), "%s/%s", fullpath, sub_items[s].name);
                        if (count < max_apps) {
                            AppEntry candidate;
                            if (parse_desktop_file(subpath, sub_items[s].name, &candidate)) {
                                /* Deduplicate by name OR exec_cmd */
                                bool dup_info = false;
                                for (size_t i = 0; i < count; i++) {
                                    if (strcasecmp(apps[i].name, candidate.name) == 0 ||
                                        strcasecmp(apps[i].exec_cmd, candidate.exec_cmd) == 0) {
                                        dup_info = true;
                                        break;
                                    }
                                }
                                if (dup_info) continue;

                                apps[count] = candidate;
                                /* Preserve launch_count */
                                if (old_entries && old_count > 0) {
                                    for (size_t o = 0; o < old_count; o++) {
                                        if (strcmp(old_entries[o].desktop_id, apps[count].desktop_id) == 0) {
                                            apps[count].launch_count = old_entries[o].launch_count;
                                            break;
                                        }
                                    }
                                }
                                resolve_and_render_icon(&apps[count]);
                                count++;
                            }
                        }
                    }
                }
            } else {
                size_t name_len = strlen(top_items[t].name);
                if (name_len > 8 && strcmp(top_items[t].name + name_len - 8, ".desktop") == 0) {
                    /* Deduplicate by desktop_id */
                    bool dup = false;
                    for (size_t i = 0; i < count; i++) {
                        if (strcmp(apps[i].desktop_id, top_items[t].name) == 0) {
                            dup = true;
                            break;
                        }
                    }
                    if (dup) continue;

                    if (count < max_apps) {
                        AppEntry candidate;
                        if (parse_desktop_file(fullpath, top_items[t].name, &candidate)) {
                            /* Deduplicate by name OR exec_cmd */
                            bool dup_info = false;
                            for (size_t i = 0; i < count; i++) {
                                if (strcasecmp(apps[i].name, candidate.name) == 0 ||
                                    strcasecmp(apps[i].exec_cmd, candidate.exec_cmd) == 0) {
                                    dup_info = true;
                                    break;
                                }
                            }
                            if (dup_info) continue;

                            apps[count] = candidate;
                            /* Preserve launch_count */
                            if (old_entries && old_count > 0) {
                                for (size_t o = 0; o < old_count; o++) {
                                    if (strcmp(old_entries[o].desktop_id, apps[count].desktop_id) == 0) {
                                        apps[count].launch_count = old_entries[o].launch_count;
                                        break;
                                    }
                                }
                            }
                            resolve_and_render_icon(&apps[count]);
                            count++;
                        }
                    }
                }
            }
        }
    }

    if (count > 0) {
        qsort(apps, count, sizeof(AppEntry), compare_apps_alphabetical);
    }

    cleanup_icon_resources();

    return count;
}

/* --- Binary Cache Implementation --- */

int cache_load_or_build(LauncherState *state, bool force_rebuild) {
    char cache_path[PATH_BUF_SIZE];
    get_cache_path(cache_path, sizeof(cache_path));

    uint64_t current_mtime_hash = compute_dir_mtime_hash();

    if (!force_rebuild) {
        int fd = open(cache_path, O_RDWR);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(CacheHeader)) {
                CacheHeader hdr;
                if (pread(fd, &hdr, sizeof(hdr), 0) == sizeof(hdr)) {
                    if (hdr.magic == CACHE_MAGIC &&
                        hdr.version == CACHE_VERSION &&
                        hdr.dir_mtime_hash == current_mtime_hash &&
                        st.st_size == (off_t)(sizeof(CacheHeader) + hdr.app_count * sizeof(AppEntry))) {
                        void *map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                        close(fd);
                        if (map != MAP_FAILED) {
                            state->apps = (AppEntry *)((uint8_t *)map + sizeof(CacheHeader));
                            state->app_count = hdr.app_count;
                            filter_apps(state);
                            return 0;
                        }
                    }
                }
            }
            close(fd);
        }
    }

    /* Need to rebuild cache */
    /* Read old cache entries if possible to preserve launch_count */
    AppEntry *old_entries = NULL;
    size_t old_count = 0;
    int old_fd = open(cache_path, O_RDONLY);
    if (old_fd >= 0) {
        struct stat st;
        if (fstat(old_fd, &st) == 0 && st.st_size >= (off_t)sizeof(CacheHeader)) {
            CacheHeader hdr;
            if (read(old_fd, &hdr, sizeof(hdr)) == sizeof(hdr) &&
                hdr.magic == CACHE_MAGIC &&
                st.st_size == (off_t)(sizeof(CacheHeader) + hdr.app_count * sizeof(AppEntry))) {
                old_entries = (AppEntry *)malloc(hdr.app_count * sizeof(AppEntry));
                if (old_entries) {
                    if (read(old_fd, old_entries, hdr.app_count * sizeof(AppEntry)) ==
                        (ssize_t)(hdr.app_count * sizeof(AppEntry))) {
                        old_count = hdr.app_count;
                    } else {
                        free(old_entries);
                        old_entries = NULL;
                    }
                }
            }
        }
        close(old_fd);
    }

    AppEntry *scanned_apps = (AppEntry *)calloc(MAX_APPS, sizeof(AppEntry));
    if (!scanned_apps) {
        if (old_entries) free(old_entries);
        return -1;
    }

    size_t count = scan_all_desktop_apps(scanned_apps, MAX_APPS, old_entries, old_count);
    if (old_entries) {
        free(old_entries);
        old_entries = NULL;
    }

    /* Write cache atomically via temporary file */
    char tmp_path[PATH_BUF_SIZE];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", cache_path, (long)getpid());

    int tmp_fd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) {
        free(scanned_apps);
        return -1;
    }

    CacheHeader hdr;
    hdr.magic = CACHE_MAGIC;
    hdr.version = CACHE_VERSION;
    hdr.dir_mtime_hash = current_mtime_hash;
    hdr.app_count = (uint32_t)count;

    if (write(tmp_fd, &hdr, sizeof(hdr)) != sizeof(hdr) ||
        (count > 0 && write(tmp_fd, scanned_apps, count * sizeof(AppEntry)) != (ssize_t)(count * sizeof(AppEntry)))) {
        close(tmp_fd);
        unlink(tmp_path);
        free(scanned_apps);
        return -1;
    }

    close(tmp_fd);
    free(scanned_apps);

    if (rename(tmp_path, cache_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    /* Now mmap the freshly built cache */
    int fd = open(cache_path, O_RDWR);
    if (fd < 0) return -1;

    size_t total_size = sizeof(CacheHeader) + count * sizeof(AppEntry);
    void *map = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (map == MAP_FAILED) return -1;

    state->apps = (AppEntry *)((uint8_t *)map + sizeof(CacheHeader));
    state->app_count = count;
    filter_apps(state);

    return 0;
}

void cache_free(LauncherState *state) {
    if (state && state->apps) {
        uint8_t *base = (uint8_t *)state->apps - sizeof(CacheHeader);
        size_t total_size = sizeof(CacheHeader) + state->app_count * sizeof(AppEntry);
        msync(base, total_size, MS_SYNC);
        munmap(base, total_size);
        state->apps = NULL;
        state->app_count = 0;
    }
}

void cache_record_launch(LauncherState *state, int app_index) {
    if (!state || !state->apps || app_index < 0 || (size_t)app_index >= state->app_count) {
        return;
    }
    state->apps[app_index].launch_count++;
    uint8_t *base = (uint8_t *)state->apps - sizeof(CacheHeader);
    size_t total_size = sizeof(CacheHeader) + state->app_count * sizeof(AppEntry);
    msync(base, total_size, MS_ASYNC);
}

/* --- Smart Filtering & Ranking --- */

typedef struct {
    int index;
    int score;
    const char *name;
} FilterItem;

static int compare_filter_items(const void *a, const void *b) {
    const FilterItem *ia = (const FilterItem *)a;
    const FilterItem *ib = (const FilterItem *)b;
    if (ia->score != ib->score) {
        return ib->score - ia->score; /* descending score */
    }
    return strcasecmp(ia->name, ib->name); /* alphabetical ascending */
}

/* Helper: check fuzzy subsequence matching */
static bool fuzzy_match(const char *haystack, const char *needle, int *out_score) {
    int score = 1000;
    const char *h = haystack;
    const char *n = needle;
    int last_idx = -1;
    int cur_idx = 0;

    while (*n && *h) {
        char ch_n = tolower((unsigned char)*n);
        char ch_h = tolower((unsigned char)*h);
        if (ch_n == ch_h) {
            if (last_idx >= 0) {
                int gap = cur_idx - last_idx - 1;
                score -= gap * 15;
            }
            last_idx = cur_idx;
            n++;
        }
        h++;
        cur_idx++;
    }

    if (*n == '\0') {
        if (score < 100) score = 100;
        *out_score = score;
        return true;
    }
    return false;
}

void filter_apps(LauncherState *state) {
    if (!state) return;

    /* Check if query is empty or whitespace only */
    bool is_empty = true;
    for (int i = 0; i < state->query_len; i++) {
        if (!isspace((unsigned char)state->query[i])) {
            is_empty = false;
            break;
        }
    }

    if (is_empty) {
        FilterItem items[MAX_APPS];
        size_t count = 0;
        for (size_t i = 0; i < state->app_count && count < MAX_APPS; i++) {
            items[count].index = (int)i;
            items[count].score = (int)state->apps[i].launch_count;
            items[count].name = state->apps[i].name;
            count++;
        }
        qsort(items, count, sizeof(FilterItem), compare_filter_items);
        state->filtered_count = count;
        for (size_t i = 0; i < count; i++) {
            state->filtered[i] = items[i].index;
        }
    } else {
        FilterItem items[MAX_APPS];
        size_t count = 0;
        size_t qlen = (size_t)state->query_len;

        for (size_t i = 0; i < state->app_count && count < MAX_APPS; i++) {
            const AppEntry *app = &state->apps[i];
            int best_score = -1;

            /* Tier 1: name starts with query (+10000) */
            if (strncasecmp(app->name, state->query, qlen) == 0) {
                best_score = 10000;
            }

            /* Tier 2: word boundary in name starts with query (+7500) */
            if (best_score < 7500) {
                const char *p = app->name;
                while (*p) {
                    if (isspace((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.') {
                        p++;
                        if (strncasecmp(p, state->query, qlen) == 0) {
                            if (7500 > best_score) best_score = 7500;
                            break;
                        }
                    } else {
                        p++;
                    }
                }
            }

            /* Tier 3: substring match in name (+5000 minus position offset) */
            if (best_score < 5000) {
                const char *pos = strcasestr_custom(app->name, state->query);
                if (pos) {
                    int offset = (int)(pos - app->name);
                    int s = 5000 - offset * 10;
                    if (s < 3100) s = 3100;
                    if (s > best_score) best_score = s;
                }
            }

            /* Tier 4: substring match in exec_cmd, generic_name, or keywords (+3000) */
            if (best_score < 3000) {
                if (strcasestr_custom(app->exec_cmd, state->query) ||
                    strcasestr_custom(app->generic_name, state->query) ||
                    strcasestr_custom(app->keywords, state->query)) {
                    best_score = 3000;
                }
            }

            /* Tier 5: fuzzy subsequence match in name (+1000 minus gap penalty) */
            if (best_score < 1000) {
                int fscore = 0;
                if (fuzzy_match(app->name, state->query, &fscore)) {
                    if (fscore > best_score) best_score = fscore;
                }
            }

            if (best_score > 0) {
                int total_score = best_score + (int)(app->launch_count * 50);
                items[count].index = (int)i;
                items[count].score = total_score;
                items[count].name = app->name;
                count++;
            }
        }

        qsort(items, count, sizeof(FilterItem), compare_filter_items);
        state->filtered_count = count;
        for (size_t i = 0; i < count; i++) {
            state->filtered[i] = items[i].index;
        }
    }

    /* Reset selection and scroll position to top when filter changes */
    state->selected_idx = 0;
    state->scroll_row = 0;

    state->needs_redraw = true;
}

/* --- $PATH Executable Check --- */

PathStatus check_path_executable(const char *query) {
    if (!query) return PATH_STATUS_EMPTY;

    while (isspace((unsigned char)*query)) query++;
    if (*query == '\0') return PATH_STATUS_EMPTY;

    /* Extract the first whitespace-delimited token supporting quotes or escapes */
    char token[PATH_MAX];
    size_t t = 0;
    const char *p = query;
    bool in_single_quote = false;
    bool in_double_quote = false;

    while (*p && t + 1 < sizeof(token)) {
        if (*p == '\\' && !in_single_quote && *(p + 1)) {
            p++;
            token[t++] = *p++;
            continue;
        }
        if (*p == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            p++;
            continue;
        }
        if (*p == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            p++;
            continue;
        }
        if (isspace((unsigned char)*p) && !in_single_quote && !in_double_quote) {
            break;
        }
        token[t++] = *p++;
    }
    token[t] = '\0';

    if (t == 0) return PATH_STATUS_EMPTY;

    static char last_token[PATH_MAX] = "";
    static PathStatus last_status = PATH_STATUS_INVALID;
    if (strcmp(token, last_token) == 0) {
        return last_status;
    }

    strncpy(last_token, token, sizeof(last_token) - 1);
    last_token[sizeof(last_token) - 1] = '\0';

    /* If token contains '/', stat directly */
    if (strchr(token, '/')) {
        struct stat st;
        if (stat(token, &st) == 0 && S_ISREG(st.st_mode) && access(token, X_OK) == 0) {
            last_status = PATH_STATUS_VALID;
            return PATH_STATUS_VALID;
        }
        last_status = PATH_STATUS_INVALID;
        return PATH_STATUS_INVALID;
    }

    /* Otherwise iterate through PATH */
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) {
        path_env = "/usr/local/bin:/usr/bin:/bin";
    }

    char path_buf[4096];
    strncpy(path_buf, path_env, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    char *saveptr = NULL;
    char *dir = strtok_r(path_buf, ":", &saveptr);
    while (dir) {
        if (dir[0]) {
            char full[PATH_BUF_SIZE];
            snprintf(full, sizeof(full), "%s/%s", dir, token);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) {
                last_status = PATH_STATUS_VALID;
                return PATH_STATUS_VALID;
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    last_status = PATH_STATUS_INVALID;
    return PATH_STATUS_INVALID;
}

/* --- Detached Process Spawning --- */

static void setup_detached_child(void) {
    /* Create new session */
    setsid();

    /* Reset standard signal handlers */
    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    /* Redirect stdin, stdout, stderr to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }

    /* Close extra file descriptors */
    int max_fd = (int)sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 1024) max_fd = 1024;
    for (int fd = 3; fd < max_fd; fd++) {
        close(fd);
    }
}

static int launch_detached_command(const char *cmd) {
    if (!cmd || cmd[0] == '\0') return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* First child: fork again for double-fork daemonizing */
        pid_t pid2 = fork();
        if (pid2 < 0) {
            _exit(1);
        }
        if (pid2 > 0) {
            _exit(0);
        }

        /* Grandchild */
        setup_detached_child();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent waits for first child */
    int status;
    waitpid(pid, &status, 0);
    return 0;
}

typedef struct {
    int exit_code;
    char msg[256];
} RawCmdReport;

/* Strip shell prefixes like "zsh:kill:1: ", "zsh:1: ", "bash: line 1: ", "sh: 1: " */
static void clean_stderr_line(const char *raw, char *out, size_t out_max) {
    if (!out || out_max == 0) return;
    out[0] = '\0';
    if (!raw) return;

    while (*raw && (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n')) {
        raw++;
    }
    if (*raw == '\0') return;

    char line[512];
    size_t l = 0;
    while (raw[l] && raw[l] != '\n' && raw[l] != '\r' && l + 1 < sizeof(line)) {
        line[l] = raw[l];
        l++;
    }
    line[l] = '\0';

    const char *p = line;

    /* Handle "zsh:..." prefixes */
    if (strncmp(p, "zsh:", 4) == 0) {
        const char *after_zsh = p + 4;
        /* Case 1: "zsh:<line>: <msg>" */
        const char *q = after_zsh;
        while (isdigit((unsigned char)*q)) q++;
        if (q > after_zsh && strncmp(q, ": ", 2) == 0) {
            p = q + 2;
        } else {
            /* Case 2: "zsh:<builtin>:<line>: <msg>" -> "<builtin>: <msg>" */
            const char *colon = strchr(after_zsh, ':');
            if (colon && colon > after_zsh) {
                const char *r = colon + 1;
                while (isdigit((unsigned char)*r)) r++;
                if (r > colon + 1 && strncmp(r, ": ", 2) == 0) {
                    size_t b_len = (size_t)(colon - after_zsh);
                    snprintf(out, out_max, "%.*s: %s", (int)b_len, after_zsh, r + 2);
                    return;
                }
            }
        }
    } else {
        /* Handle "<shell>: line <N>: <msg>" or "<shell>: <N>: <msg>" */
        const char *colon = strchr(p, ':');
        if (colon && colon[1] == ' ') {
            const char *after_c = colon + 2;
            if (strncmp(after_c, "line ", 5) == 0) {
                const char *q = after_c + 5;
                while (isdigit((unsigned char)*q)) q++;
                if (q > after_c + 5 && strncmp(q, ": ", 2) == 0) {
                    p = q + 2;
                }
            } else if (isdigit((unsigned char)*after_c)) {
                const char *q = after_c;
                while (isdigit((unsigned char)*q)) q++;
                if (strncmp(q, ": ", 2) == 0) {
                    p = q + 2;
                }
            }
        }
    }

    strncpy(out, p, out_max - 1);
    out[out_max - 1] = '\0';
}

static void exec_user_shell(const char *cmd) {
    const char *shell = getenv("SHELL");
    if (shell && shell[0] == '/' && access(shell, X_OK) == 0) {
        const char *base = strrchr(shell, '/');
        base = base ? (base + 1) : shell;
        execl(shell, base, "-c", cmd, (char *)NULL);
    }
    if (access("/bin/bash", X_OK) == 0) {
        execl("/bin/bash", "bash", "-c", cmd, (char *)NULL);
    }
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
}

int launch_raw_command(const char *cmd, char *err_out, size_t err_out_size) {
    if (err_out && err_out_size > 0) {
        err_out[0] = '\0';
    }
    if (!cmd || cmd[0] == '\0') return -1;

    int status_pipe[2];
    if (pipe2(status_pipe, O_CLOEXEC) != 0) {
        return launch_detached_command(cmd);
    }

    pid_t pid1 = fork();
    if (pid1 < 0) {
        close(status_pipe[0]);
        close(status_pipe[1]);
        return -1;
    }

    if (pid1 == 0) {
        /* First child: double-fork monitor so it is reparented to init immediately */
        close(status_pipe[0]);

        pid_t pid2 = fork();
        if (pid2 < 0) {
            _exit(1);
        }
        if (pid2 > 0) {
            _exit(0);
        }

        /* Monitor process (grandchild) */
        setsid();
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);

        int err_pipe[2];
        if (pipe2(err_pipe, O_CLOEXEC) != 0) {
            close(status_pipe[1]);
            _exit(1);
        }

        pid_t cmd_pid = fork();
        if (cmd_pid < 0) {
            close(err_pipe[0]);
            close(err_pipe[1]);
            close(status_pipe[1]);
            _exit(1);
        }

        if (cmd_pid == 0) {
            /* Command process */
            signal(SIGHUP, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);

            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                if (devnull > 2) close(devnull);
            }
            dup2(err_pipe[1], STDERR_FILENO);

            int max_fd = (int)sysconf(_SC_OPEN_MAX);
            if (max_fd < 0 || max_fd > 1024) max_fd = 1024;
            for (int fd = 3; fd < max_fd; fd++) {
                close(fd);
            }

            exec_user_shell(cmd);
        }

        /* Monitor process continues */
        close(err_pipe[1]);

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            if (err_pipe[0] != STDIN_FILENO && status_pipe[1] != STDIN_FILENO)
                dup2(devnull, STDIN_FILENO);
            if (err_pipe[0] != STDOUT_FILENO && status_pipe[1] != STDOUT_FILENO)
                dup2(devnull, STDOUT_FILENO);
            if (err_pipe[0] != STDERR_FILENO && status_pipe[1] != STDERR_FILENO)
                dup2(devnull, STDERR_FILENO);
            if (devnull > 2 && devnull != err_pipe[0] && devnull != status_pipe[1])
                close(devnull);
        }

        int max_fd = (int)sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 1024) max_fd = 1024;
        for (int fd = 3; fd < max_fd; fd++) {
            if (fd != err_pipe[0] && fd != status_pipe[1]) {
                close(fd);
            }
        }

        int flags = fcntl(err_pipe[0], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(err_pipe[0], F_SETFL, flags | O_NONBLOCK);
        }

        int pidfd = -1;
#ifdef SYS_pidfd_open
        pidfd = (int)syscall(SYS_pidfd_open, cmd_pid, 0);
#endif

        char raw_err[512];
        size_t raw_err_len = 0;
        raw_err[0] = '\0';

        int cmd_status = 0;
        for (;;) {
            struct pollfd pfds[2];
            pfds[0].fd = err_pipe[0];
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            pfds[1].fd = pidfd;
            pfds[1].events = (pidfd >= 0) ? POLLIN : 0;
            pfds[1].revents = 0;

            int timeout_ms = (pidfd >= 0) ? -1 : 15;
            poll(pfds, (pidfd >= 0) ? 2 : 1, timeout_ms);

            /* Drain available stderr bytes without blocking */
            char discard[256];
            for (;;) {
                ssize_t n = read(err_pipe[0], discard, sizeof(discard));
                if (n > 0) {
                    if (raw_err_len + 1 < sizeof(raw_err)) {
                        size_t copy_n = (size_t)n;
                        if (raw_err_len + copy_n >= sizeof(raw_err)) {
                            copy_n = sizeof(raw_err) - 1 - raw_err_len;
                        }
                        memcpy(raw_err + raw_err_len, discard, copy_n);
                        raw_err_len += copy_n;
                        raw_err[raw_err_len] = '\0';
                    }
                } else {
                    break;
                }
            }

            pid_t w = waitpid(cmd_pid, &cmd_status, WNOHANG);
            if (w == cmd_pid) {
                /* Final non-blocking drain after child exit */
                for (;;) {
                    ssize_t n = read(err_pipe[0], discard, sizeof(discard));
                    if (n > 0 && raw_err_len + 1 < sizeof(raw_err)) {
                        size_t copy_n = (size_t)n;
                        if (raw_err_len + copy_n >= sizeof(raw_err)) {
                            copy_n = sizeof(raw_err) - 1 - raw_err_len;
                        }
                        memcpy(raw_err + raw_err_len, discard, copy_n);
                        raw_err_len += copy_n;
                        raw_err[raw_err_len] = '\0';
                    } else {
                        break;
                    }
                }
                break;
            } else if (w < 0 && errno != EINTR) {
                break;
            }
        }

        close(err_pipe[0]);
        if (pidfd >= 0) close(pidfd);

        int exit_code = 0;
        if (WIFEXITED(cmd_status) && WEXITSTATUS(cmd_status) != 0) {
            exit_code = WEXITSTATUS(cmd_status);
        } else if (WIFSIGNALED(cmd_status)) {
            int sig = WTERMSIG(cmd_status);
            if (sig != SIGTERM && sig != SIGINT && sig != SIGHUP &&
                sig != SIGQUIT && sig != SIGPIPE) {
                exit_code = 128 + sig;
            }
        }

        if (exit_code != 0) {
            char cleaned[220];
            clean_stderr_line(raw_err, cleaned, sizeof(cleaned));

            RawCmdReport pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.exit_code = exit_code;
            if (cleaned[0] != '\0') {
                snprintf(pkt.msg, sizeof(pkt.msg), "Error (code %d) — %s", exit_code, cleaned);
            } else {
                snprintf(pkt.msg, sizeof(pkt.msg), "Error (code %d)", exit_code);
            }

            ssize_t nw = write(status_pipe[1], &pkt, sizeof(pkt));
            close(status_pipe[1]);

            if (nw != (ssize_t)sizeof(pkt)) {
                /* Launcher GUI already closed after 100ms timeout: send notification */
                char summary[128];
                char body[512];
                snprintf(summary, sizeof(summary), "Error (code %d)", exit_code);
                if (cleaned[0] != '\0') {
                    snprintf(body, sizeof(body), "%s\n%s", cmd, cleaned);
                } else {
                    snprintf(body, sizeof(body), "%s", cmd);
                }
                execlp("notify-send", "notify-send", "-u", "critical", "-a", "launcher",
                       "-i", "utilities-terminal", summary, body, (char *)NULL);
            }
            _exit(0);
        }

        close(status_pipe[1]);
        _exit(0);
    }

    /* Parent (Launcher) */
    close(status_pipe[1]);
    int st;
    waitpid(pid1, &st, 0);

    /* Wait up to 100ms for fast command completion/failure */
    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    const int timeout_total_ms = 100;

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        long elapsed_ms = (ts_now.tv_sec - ts_start.tv_sec) * 1000L +
                          (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000L;
        int rem_ms = timeout_total_ms - (int)elapsed_ms;
        if (rem_ms <= 0) break;

        struct pollfd pfd;
        pfd.fd = status_pipe[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, rem_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            /* Timeout reached */
            break;
        }
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            RawCmdReport pkt;
            ssize_t nr = read(status_pipe[0], &pkt, sizeof(pkt));
            close(status_pipe[0]);
            if (nr == (ssize_t)sizeof(pkt) && pkt.exit_code != 0) {
                if (err_out && err_out_size > 0) {
                    strncpy(err_out, pkt.msg, err_out_size - 1);
                    err_out[err_out_size - 1] = '\0';
                }
                return pkt.exit_code;
            }
            return 0;
        }
    }

    /* Final non-blocking check before closing status_pipe[0] */
    int fl = fcntl(status_pipe[0], F_GETFL, 0);
    if (fl >= 0) {
        fcntl(status_pipe[0], F_SETFL, fl | O_NONBLOCK);
    }
    RawCmdReport pkt;
    ssize_t nr = read(status_pipe[0], &pkt, sizeof(pkt));
    close(status_pipe[0]);
    if (nr == (ssize_t)sizeof(pkt) && pkt.exit_code != 0) {
        if (err_out && err_out_size > 0) {
            strncpy(err_out, pkt.msg, err_out_size - 1);
            err_out[err_out_size - 1] = '\0';
        }
        return pkt.exit_code;
    }

    return 0;
}

static const char *find_terminal_emulator(void) {
    const char *term = getenv("TERMINAL");
    if (term && term[0] && check_path_executable(term) == PATH_STATUS_VALID) {
        return term;
    }

    static const char *candidates[] = {
        "foot",
        "alacritty",
        "kitty",
        "wezterm",
        "gnome-terminal",
        "xterm"
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (check_path_executable(candidates[i]) == PATH_STATUS_VALID) {
            return candidates[i];
        }
    }
    return NULL;
}

int launch_desktop_app(const AppEntry *app) {
    if (!app || app->exec_cmd[0] == '\0') return -1;

    if (!app->terminal) {
        return launch_detached_command(app->exec_cmd);
    }

    /* Terminal application requested */
    const char *term = find_terminal_emulator();
    if (!term) {
        /* Fallback: run directly if no terminal emulator found */
        return launch_detached_command(app->exec_cmd);
    }

    char term_cmd[1024];
    snprintf(term_cmd, sizeof(term_cmd), "%s -e sh -c \"%s\"", term, app->exec_cmd);
    return launch_detached_command(term_cmd);
}
