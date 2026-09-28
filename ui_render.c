#define _GNU_SOURCE
#include "launcher.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NON_ASCII_CACHE_SIZE 256

typedef struct {
    uint32_t codepoint;
    int font_id; // 0 for main/textbox font, 1 for small/app label font
    int width;
    int height;
    int left;
    int top;
    int advance_x;
    uint8_t *bitmap;
} CachedGlyph;

static FT_Library s_ft = NULL;
static FT_Face s_face_main = NULL;
static FT_Face s_face_small = NULL;

/* O(1) Fast ASCII table: font_id 0 and 1, codepoint 0..127 */
static CachedGlyph s_ascii_cache[2][128];
static bool s_ascii_valid[2][128];

/* Non-ASCII open-addressed / hash cache */
static CachedGlyph s_non_ascii_cache[NON_ASCII_CACHE_SIZE];
static bool s_non_ascii_valid[NON_ASCII_CACHE_SIZE];

/* Helper clamp */
static inline int clamp_i(int v, int min_val, int max_val) {
    if (v < min_val) return min_val;
    if (v > max_val) return max_val;
    return v;
}

/* Recursive font search helper prioritizing FiraMono first */
static int find_font_in_dir(const char *dir_path, char *out_path, size_t out_size) {
    DIR *d = opendir(dir_path);
    if (!d) return 0;

    struct dirent *ent;
    char fallback_subpath[1024] = "";

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char subpath[1024];
        if (snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, ent->d_name) >= (int)sizeof(subpath))
            continue;

        struct stat st;
        if (stat(subpath, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (find_font_in_dir(subpath, out_path, out_size)) {
                closedir(d);
                return 1;
            }
        } else if (S_ISREG(st.st_mode)) {
            const char *dot = strrchr(ent->d_name, '.');
            if (dot && (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0)) {
                /* Strictly check FiraMono first */
                if (strcasestr(ent->d_name, "FiraMono")) {
                    strncpy(out_path, subpath, out_size - 1);
                    out_path[out_size - 1] = '\0';
                    closedir(d);
                    return 1;
                }
                if (fallback_subpath[0] == '\0') {
                    if (strcasestr(ent->d_name, "Mono") || strcasestr(ent->d_name, "Sans")) {
                        strncpy(fallback_subpath, subpath, sizeof(fallback_subpath) - 1);
                        fallback_subpath[sizeof(fallback_subpath) - 1] = '\0';
                    }
                }
            }
        }
    }
    closedir(d);

    if (fallback_subpath[0] != '\0') {
        strncpy(out_path, fallback_subpath, out_size - 1);
        out_path[out_size - 1] = '\0';
        return 1;
    }
    return 0;
}

static int locate_font(const char *preferred_font, char *out_path, size_t out_size) {
    if (preferred_font && preferred_font[0] != '\0') {
        if (access(preferred_font, R_OK) == 0) {
            strncpy(out_path, preferred_font, out_size - 1);
            out_path[out_size - 1] = '\0';
            return 1;
        }
    }

    const char *home = getenv("HOME");
    char candidate[1024];

    if (home) {
        snprintf(candidate, sizeof(candidate), "%s/.fonts/fira/FiraMono-Regular.ttf", home);
        if (access(candidate, R_OK) == 0) {
            strncpy(out_path, candidate, out_size - 1);
            out_path[out_size - 1] = '\0';
            return 1;
        }
        snprintf(candidate, sizeof(candidate), "%s/.fonts/fira/FiraMono-Medium.ttf", home);
        if (access(candidate, R_OK) == 0) {
            strncpy(out_path, candidate, out_size - 1);
            out_path[out_size - 1] = '\0';
            return 1;
        }
    }

    const char *standard_paths[] = {
        "/usr/share/fonts/truetype/fira/FiraMono-Regular.ttf",
        "/usr/share/fonts/opentype/fira/FiraMono-Regular.otf",
        "/usr/share/fonts/opentype/firacode/FiraCode-Regular.otf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
    };

    for (size_t i = 0; i < sizeof(standard_paths) / sizeof(standard_paths[0]); i++) {
        if (access(standard_paths[i], R_OK) == 0) {
            strncpy(out_path, standard_paths[i], out_size - 1);
            out_path[out_size - 1] = '\0';
            return 1;
        }
    }

    /* Recursive scan in ~/.fonts, ~/.local/share/fonts, /usr/share/fonts */
    if (home) {
        snprintf(candidate, sizeof(candidate), "%s/.fonts", home);
        if (find_font_in_dir(candidate, out_path, out_size)) return 1;

        snprintf(candidate, sizeof(candidate), "%s/.local/share/fonts", home);
        if (find_font_in_dir(candidate, out_path, out_size)) return 1;
    }

    if (find_font_in_dir("/usr/share/fonts", out_path, out_size)) return 1;

    return 0;
}

int ui_init(const char *preferred_font, int font_size_px) {
    if (FT_Init_FreeType(&s_ft)) {
        return -1;
    }

    char font_path[1024];
    if (!locate_font(preferred_font, font_path, sizeof(font_path))) {
        FT_Done_FreeType(s_ft);
        s_ft = NULL;
        return -1;
    }

    if (FT_New_Face(s_ft, font_path, 0, &s_face_main)) {
        FT_Done_FreeType(s_ft);
        s_ft = NULL;
        return -1;
    }

    if (FT_New_Face(s_ft, font_path, 0, &s_face_small)) {
        FT_Done_Face(s_face_main);
        s_face_main = NULL;
        FT_Done_FreeType(s_ft);
        s_ft = NULL;
        return -1;
    }

    int main_px = font_size_px > 0 ? font_size_px : 22;
    int small_px = (int)roundf(main_px * 0.68f);
    if (small_px < 11) small_px = 11;

    FT_Set_Pixel_Sizes(s_face_main, 0, main_px);
    FT_Set_Pixel_Sizes(s_face_small, 0, small_px);

    memset(s_ascii_valid, 0, sizeof(s_ascii_valid));
    memset(s_non_ascii_valid, 0, sizeof(s_non_ascii_valid));

    return 0;
}

void ui_cleanup(void) {
    for (int f = 0; f < 2; f++) {
        for (int c = 0; c < 128; c++) {
            if (s_ascii_valid[f][c]) {
                free(s_ascii_cache[f][c].bitmap);
                s_ascii_cache[f][c].bitmap = NULL;
                s_ascii_valid[f][c] = false;
            }
        }
    }

    for (int i = 0; i < NON_ASCII_CACHE_SIZE; i++) {
        if (s_non_ascii_valid[i]) {
            free(s_non_ascii_cache[i].bitmap);
            s_non_ascii_cache[i].bitmap = NULL;
            s_non_ascii_valid[i] = false;
        }
    }

    if (s_face_main) {
        FT_Done_Face(s_face_main);
        s_face_main = NULL;
    }
    if (s_face_small) {
        FT_Done_Face(s_face_small);
        s_face_small = NULL;
    }
    if (s_ft) {
        FT_Done_FreeType(s_ft);
        s_ft = NULL;
    }
}

static uint32_t decode_utf8(const char **pstr) {
    const unsigned char *s = (const unsigned char *)*pstr;
    if (*s == 0) return 0;

    uint32_t codepoint = 0;
    int len = 0;

    if (*s < 0x80) {
        codepoint = *s;
        len = 1;
    } else if ((*s & 0xE0) == 0xC0) {
        codepoint = *s & 0x1F;
        len = 2;
    } else if ((*s & 0xF0) == 0xE0) {
        codepoint = *s & 0x0F;
        len = 3;
    } else if ((*s & 0xF8) == 0xF0) {
        codepoint = *s & 0x07;
        len = 4;
    } else {
        *pstr += 1;
        return 0xFFFD;
    }

    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *pstr += 1;
            return 0xFFFD;
        }
        codepoint = (codepoint << 6) | (s[i] & 0x3F);
    }

    *pstr += len;
    return codepoint;
}

static CachedGlyph *get_glyph(int font_id, uint32_t codepoint) {
    if (codepoint < 128) {
        if (s_ascii_valid[font_id][codepoint]) {
            return &s_ascii_cache[font_id][codepoint];
        }
    } else {
        uint32_t h = (codepoint ^ ((uint32_t)font_id << 16)) % NON_ASCII_CACHE_SIZE;
        if (s_non_ascii_valid[h] &&
            s_non_ascii_cache[h].font_id == font_id &&
            s_non_ascii_cache[h].codepoint == codepoint) {
            return &s_non_ascii_cache[h];
        }
    }

    FT_Face face = (font_id == 0) ? s_face_main : s_face_small;
    if (!face) return NULL;

    FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
    if (glyph_index == 0 && codepoint != ' ') {
        glyph_index = FT_Get_Char_Index(face, '?');
    }

    if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT)) return NULL;
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return NULL;

    CachedGlyph *entry = NULL;
    if (codepoint < 128) {
        entry = &s_ascii_cache[font_id][codepoint];
        s_ascii_valid[font_id][codepoint] = true;
    } else {
        uint32_t h = (codepoint ^ ((uint32_t)font_id << 16)) % NON_ASCII_CACHE_SIZE;
        entry = &s_non_ascii_cache[h];
        if (s_non_ascii_valid[h]) {
            free(entry->bitmap);
            entry->bitmap = NULL;
        }
        s_non_ascii_valid[h] = true;
    }

    entry->font_id = font_id;
    entry->codepoint = codepoint;
    entry->width = face->glyph->bitmap.width;
    entry->height = face->glyph->bitmap.rows;
    entry->left = face->glyph->bitmap_left;
    entry->top = face->glyph->bitmap_top;
    entry->advance_x = face->glyph->advance.x >> 6;

    int size = entry->width * entry->height;
    if (size > 0) {
        entry->bitmap = (uint8_t *)malloc(size);
        if (entry->bitmap) {
            uint8_t *src = face->glyph->bitmap.buffer;
            int pitch = face->glyph->bitmap.pitch;
            for (int r = 0; r < entry->height; r++) {
                memcpy(&entry->bitmap[r * entry->width], &src[r * pitch], entry->width);
            }
        }
    } else {
        entry->bitmap = NULL;
    }

    return entry;
}

void ui_compute_layout(LauncherState *state, uint32_t width, uint32_t height) {
    state->screen_w = width;
    state->screen_h = height;

    LayoutMetrics *l = &state->layout;

    /* 1. Top-center textbox: clean fixed top margin (36px), 54px height */
    l->textbox_y = 36;
    l->textbox_h = 54;
    l->textbox_w = clamp_i((int)(width * 0.38f), 420, 840);
    if (l->textbox_w > (int)width - 40) l->textbox_w = (int)width - 40;
    if (l->textbox_w < 200) l->textbox_w = (int)width > 40 ? (int)width - 40 : (int)width;
    l->textbox_x = ((int)width - l->textbox_w) / 2;

    /* 2. Tile dimensions and fixed comfortable spacing */
    l->cell_w = 160;
    l->cell_h = 132;
    l->cell_pad_x = 28;
    l->cell_pad_y = 24;

    /* Compute maximum columns and rows that physically fit on the screen */
    int margin_x = 48;
    int grid_top = l->textbox_y + l->textbox_h + 44;
    int margin_bottom = 40;
    int avail_w = (int)width - 2 * margin_x;
    int avail_h = (int)height - grid_top - margin_bottom;
    int max_cols = (avail_w + l->cell_pad_x) / (l->cell_w + l->cell_pad_x);
    int max_rows = (avail_h + l->cell_pad_y) / (l->cell_h + l->cell_pad_y);
    if (max_cols < 1) max_cols = 1;
    if (max_rows < 1) max_rows = 1;

    /* Choose cols and visible_rows preferring a square-ish shape */
    int n = (int)(state->app_count > 0 ? state->app_count : 1);
    int side = (int)ceilf(sqrtf((float)n));
    if (side < 1) side = 1;

    int cols;
    if (side <= max_rows && side <= max_cols) {
        /* A square-ish grid (e.g. 8x8 or 9x9) fits all apps on the display! */
        cols = side;
    } else if (side > max_rows) {
        /* Square grid would exceed screen height: widen cols just enough to fit within max_rows, up to max_cols */
        int needed_cols = (n + max_rows - 1) / max_rows;
        cols = clamp_i(needed_cols, 1, max_cols);
    } else {
        cols = max_cols;
    }

    int total_rows_for_all = (n + cols - 1) / cols;
    int visible_rows = clamp_i(total_rows_for_all, 1, max_rows);
    l->cols = cols;
    l->visible_rows = visible_rows;

    /* Center the grid horizontally and give it a balanced vertical position */
    int total_grid_w = cols * l->cell_w + (cols - 1) * l->cell_pad_x;
    int total_grid_h = visible_rows * l->cell_h + (visible_rows - 1) * l->cell_pad_y;
    l->grid_x = ((int)width - total_grid_w) / 2;
    int extra_h = avail_h - total_grid_h;
    l->grid_y = grid_top + (extra_h > 0 ? extra_h / 4 : 0);

    if (state->filtered_count > 0) {
        state->selected_idx = clamp_i(state->selected_idx, 0, (int)state->filtered_count - 1);
        int total_rows = ((int)state->filtered_count + cols - 1) / cols;
        int max_scroll = total_rows - visible_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (state->scroll_row > max_scroll) state->scroll_row = max_scroll;

        int sel_row = state->selected_idx / cols;
        if (sel_row < state->scroll_row) {
            state->scroll_row = sel_row;
        } else if (sel_row >= state->scroll_row + visible_rows) {
            state->scroll_row = sel_row - visible_rows + 1;
        }
    } else {
        state->scroll_row = 0;
        state->selected_idx = 0;
    }
    if (state->scroll_row < 0) state->scroll_row = 0;
}

int ui_grid_index_at(const LauncherState *state, int x, int y) {
    const LayoutMetrics *l = &state->layout;
    if (l->cols <= 0 || l->visible_rows <= 0) return -1;

    for (int r = 0; r < l->visible_rows; r++) {
        for (int c = 0; c < l->cols; c++) {
            int cx = l->grid_x + c * (l->cell_w + l->cell_pad_x);
            int cy = l->grid_y + r * (l->cell_h + l->cell_pad_y);

            if (x >= cx && x < cx + l->cell_w && y >= cy && y < cy + l->cell_h) {
                int filtered_idx = (state->scroll_row + r) * l->cols + c;
                if (filtered_idx >= 0 && (size_t)filtered_idx < state->filtered_count) {
                    return filtered_idx;
                }
                return -1;
            }
        }
    }
    return -1;
}

bool ui_is_in_textbox(const LauncherState *state, int x, int y) {
    const LayoutMetrics *l = &state->layout;
    return (x >= l->textbox_x && x < l->textbox_x + l->textbox_w &&
            y >= l->textbox_y && y < l->textbox_y + l->textbox_h);
}

/* Premultiplied alpha blending of source pixel (src) over destination pixel (*dst) */
static inline void blend_pixel(uint32_t *dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 0) return;
    if (sa == 255) {
        *dst = src;
        return;
    }

    uint32_t d = *dst;
    uint32_t da = (d >> 24) & 0xFF;
    uint32_t dr = (d >> 16) & 0xFF;
    uint32_t dg = (d >> 8) & 0xFF;
    uint32_t db = d & 0xFF;

    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = src & 0xFF;

    uint32_t inv_sa = 255 - sa;
    uint32_t out_a = sa + (da * inv_sa + 127) / 255;
    uint32_t out_r = sr + (dr * inv_sa + 127) / 255;
    uint32_t out_g = sg + (dg * inv_sa + 127) / 255;
    uint32_t out_b = sb + (db * inv_sa + 127) / 255;

    *dst = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static inline void blend_color_alpha(uint32_t *dst, uint32_t rgb, uint32_t alpha) {
    if (alpha == 0) return;
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;

    uint32_t premul_r = (r * alpha + 127) / 255;
    uint32_t premul_g = (g * alpha + 127) / 255;
    uint32_t premul_b = (b * alpha + 127) / 255;
    uint32_t src = (alpha << 24) | (premul_r << 16) | (premul_g << 8) | premul_b;
    blend_pixel(dst, src);
}

/* Optimized draw_rounded_rect:
 * Precomputes fill_src and border_src once.
 * Skips sqrtf and float math outside corner regions.
 */
static void draw_rounded_rect(uint32_t *buf, int width, int height,
                              int rx, int ry, int rw, int rh, int radius,
                              uint32_t fill_color, uint32_t border_color, int border_width) {
    int x0 = rx > 0 ? rx : 0;
    int y0 = ry > 0 ? ry : 0;
    int x1 = (rx + rw < width) ? rx + rw : width;
    int y1 = (ry + rh < height) ? ry + rh : height;

    uint32_t fill_a = (fill_color >> 24) & 0xFF;
    uint32_t border_a = (border_color >> 24) & 0xFF;

    /* Precompute premultiplied full ARGB values */
    uint32_t fill_src = 0;
    if (fill_a > 0) {
        uint32_t fr = (((fill_color >> 16) & 0xFF) * fill_a + 127) / 255;
        uint32_t fg = (((fill_color >> 8) & 0xFF) * fill_a + 127) / 255;
        uint32_t fb = ((fill_color & 0xFF) * fill_a + 127) / 255;
        fill_src = (fill_a << 24) | (fr << 16) | (fg << 8) | fb;
    }

    uint32_t border_src = 0;
    if (border_a > 0 && border_width > 0) {
        uint32_t br = (((border_color >> 16) & 0xFF) * border_a + 127) / 255;
        uint32_t bg = (((border_color >> 8) & 0xFF) * border_a + 127) / 255;
        uint32_t bb = ((border_color & 0xFF) * border_a + 127) / 255;
        border_src = (border_a << 24) | (br << 16) | (bg << 8) | bb;
    }

    /* Pre-blend fill and border over backdrop 0xEB16181E for direct memory stores */
    uint32_t blended_fill = 0xEB16181E;
    if (fill_a > 0) {
        blend_pixel(&blended_fill, fill_src);
    }

    uint32_t blended_border = 0xEB16181E;
    if (border_a > 0 && border_width > 0) {
        blend_pixel(&blended_border, border_src);
    }

    for (int y = y0; y < y1; y++) {
        int dy = 0;
        if (y < ry + radius) dy = ry + radius - y;
        else if (y >= ry + rh - radius) dy = y - (ry + rh - radius - 1);

        int inset_y = (y - ry < ry + rh - 1 - y) ? (y - ry) : (ry + rh - 1 - y);
        uint32_t *row_ptr = &buf[y * width];

        for (int x = x0; x < x1; x++) {
            int dx = 0;
            if (x < rx + radius) dx = rx + radius - x;
            else if (x >= rx + rw - radius) dx = x - (rx + rw - radius - 1);

            /* Fast path: not in a corner - direct memory store! */
            if (dx == 0 || dy == 0) {
                if (border_width > 0 && border_a > 0) {
                    int inset_x = (x - rx < rx + rw - 1 - x) ? (x - rx) : (rx + rw - 1 - x);
                    if (inset_x < border_width || inset_y < border_width) {
                        row_ptr[x] = blended_border;
                        continue;
                    }
                }
                if (fill_a > 0) {
                    row_ptr[x] = blended_fill;
                }
                continue;
            }

            /* Corner region: compute float distance */
            float dist_outer = sqrtf((float)(dx * dx + dy * dy)) - (float)radius;
            if (dist_outer > 0.5f) {
                continue; // outside
            }

            float edge_alpha = 1.0f;
            if (dist_outer > -0.5f) {
                edge_alpha = 0.5f - dist_outer;
                if (edge_alpha < 0.0f) edge_alpha = 0.0f;
                if (edge_alpha > 1.0f) edge_alpha = 1.0f;
            }

            bool is_border = false;
            if (border_width > 0 && border_a > 0) {
                float dist_from_corner = sqrtf((float)(dx * dx + dy * dy));
                if (dist_from_corner >= (float)(radius - border_width)) {
                    is_border = true;
                }
            }

            uint32_t *p = &row_ptr[x];
            if (is_border) {
                uint32_t a = (uint32_t)(border_a * edge_alpha);
                blend_color_alpha(p, border_color, a);
            } else if (fill_a > 0) {
                uint32_t a = (uint32_t)(fill_a * edge_alpha);
                blend_color_alpha(p, fill_color, a);
            }
        }
    }
}

/* Measure UTF-8 string width in pixels */
static int measure_text_width(int font_id, const char *str) {
    if (!str) return 0;
    int w = 0;
    const char *p = str;
    while (*p) {
        uint32_t cp = decode_utf8(&p);
        CachedGlyph *g = get_glyph(font_id, cp);
        if (g) {
            w += g->advance_x;
        }
    }
    return w;
}

/* Render text */
static void render_text(uint32_t *buf, int width, int height,
                        int font_id, const char *str, int start_x, int baseline_y,
                        uint32_t color) {
    if (!str) return;
    int cur_x = start_x;
    const char *p = str;

    while (*p) {
        uint32_t cp = decode_utf8(&p);
        CachedGlyph *g = get_glyph(font_id, cp);
        if (!g) continue;

        if (g->bitmap && g->width > 0 && g->height > 0) {
            int gx = cur_x + g->left;
            int gy = baseline_y - g->top;

            for (int r = 0; r < g->height; r++) {
                int py = gy + r;
                if (py < 0 || py >= height) continue;

                for (int c = 0; c < g->width; c++) {
                    int px = gx + c;
                    if (px < 0 || px >= width) continue;

                    uint8_t a = g->bitmap[r * g->width + c];
                    if (a > 0) {
                        blend_color_alpha(&buf[py * width + px], color, a);
                    }
                }
            }
        }
        cur_x += g->advance_x;
    }
}

/* Blit 64x64 icon pixels (ARGB8888 premultiplied) using alpha blending */
static void blit_icon(uint32_t *buf, int width, int height,
                      const uint32_t *icon, int dst_x, int dst_y) {
    for (int y = 0; y < ICON_SIZE; y++) {
        int py = dst_y + y;
        if (py < 0 || py >= height) continue;

        for (int x = 0; x < ICON_SIZE; x++) {
            int px = dst_x + x;
            if (px < 0 || px >= width) continue;

            uint32_t src = icon[y * ICON_SIZE + x];
            blend_pixel(&buf[py * width + px], src);
        }
    }
}

void ui_render_frame(LauncherState *state, uint32_t *argb_buf, uint32_t width, uint32_t height) {
    if (!argb_buf || width == 0 || height == 0) return;

    static uint32_t *s_last_buf = NULL;
    static uint32_t s_last_w = 0, s_last_h = 0;

    uint32_t backdrop_pixel = 0xEB16181E;
    const LayoutMetrics *l = &state->layout;

    bool needs_full_fill = (argb_buf != s_last_buf || width != s_last_w || height != s_last_h || argb_buf[0] != backdrop_pixel);

    if (needs_full_fill) {
        size_t total_px = (size_t)width * height;
        for (size_t i = 0; i < total_px; i++) {
            argb_buf[i] = backdrop_pixel;
        }
        s_last_buf = argb_buf;
        s_last_w = width;
        s_last_h = height;
    } else {
        /* Only clear textbox, error banner, and grid bounding boxes back to 0xEB16181E */
        int tb_x0 = clamp_i(l->textbox_x - 4, 0, (int)width);
        int tb_x1 = clamp_i(l->textbox_x + l->textbox_w + 4, 0, (int)width);
        int tb_y0 = clamp_i(l->textbox_y - 4, 0, (int)height);
        int tb_y1 = clamp_i(l->textbox_y + l->textbox_h + 46, 0, (int)height);

        for (int y = tb_y0; y < tb_y1; y++) {
            uint32_t *row = &argb_buf[y * width];
            for (int x = tb_x0; x < tb_x1; x++) {
                row[x] = backdrop_pixel;
            }
        }

        if (l->cols > 0 && l->visible_rows > 0) {
            int total_grid_w = l->cols * l->cell_w + (l->cols - 1) * l->cell_pad_x;
            int total_grid_h = l->visible_rows * l->cell_h + (l->visible_rows - 1) * l->cell_pad_y;

            int g_x0 = clamp_i(l->grid_x - 4, 0, (int)width);
            int g_x1 = clamp_i(l->grid_x + total_grid_w + 4, 0, (int)width);
            int g_y0 = clamp_i(l->grid_y - 4, 0, (int)height);
            int g_y1 = clamp_i(l->grid_y + total_grid_h + 4, 0, (int)height);

            for (int y = g_y0; y < g_y1; y++) {
                uint32_t *row = &argb_buf[y * width];
                for (int x = g_x0; x < g_x1; x++) {
                    row[x] = backdrop_pixel;
                }
            }
        }
    }

    /* 1. TOP-CENTER TEXTBOX */
    uint32_t border_col = 0xFF586875;
    if (state->error_msg[0] != '\0') {
        border_col = 0xFFE74C3C; // bright red on command error
    } else if (state->path_status == PATH_STATUS_VALID) {
        border_col = 0xFF2ECC71; // bright green
    } else if (state->path_status == PATH_STATUS_INVALID && state->filtered_count == 0) {
        border_col = 0xFFE74C3C; // bright red
    } else {
        if (state->focus == FOCUS_TEXTBOX) {
            border_col = 0xFF6495ED; // cornflower blue
        } else {
            border_col = 0xFF586875; // neutral
        }
    }

    /* Textbox background: #2d303b (0xFF2D303B), radius 6px, border 2px */
    draw_rounded_rect(argb_buf, width, height,
                      l->textbox_x, l->textbox_y, l->textbox_w, l->textbox_h,
                      6, 0xFF2D303B, border_col, 2);

    /* Textbox text & cursor */
    int text_padding_left = 18;
    int text_x = l->textbox_x + text_padding_left;
    int baseline_y = l->textbox_y + 35; // vertically centered in 56px height

    int cursor_pixel_offset = 0;
    if (state->query_len > 0) {
        /* Compute cursor pixel offset */
        char prefix[MAX_QUERY];
        int cpos = clamp_i(state->cursor_pos, 0, state->query_len);
        memcpy(prefix, state->query, cpos);
        prefix[cpos] = '\0';
        cursor_pixel_offset = measure_text_width(0, prefix);

        /* Render query text: #ddccbb (0xFFDDCCBB) */
        render_text(argb_buf, width, height, 0, state->query, text_x, baseline_y, 0xFFDDCCBB);
    } else {
        /* Placeholder text: #6c7380 */
        const char *placeholder = "Search apps or run command...";
        render_text(argb_buf, width, height, 0, placeholder, text_x, baseline_y, 0xFF6C7380);
    }

    /* Render Cursor */
    int cur_x = text_x + cursor_pixel_offset;
    int cur_y = l->textbox_y + 14;
    int cur_h = 28;

    if (state->mode == VIM_MODE_INSERT) {
        /* 2px vertical bar: #f9f9f9 (0xFFF9F9F9) */
        for (int y = cur_y; y < cur_y + cur_h && y < (int)height; y++) {
            for (int x = cur_x; x < cur_x + 2 && x < (int)width; x++) {
                if (x >= 0 && y >= 0) argb_buf[y * width + x] = 0xFFF9F9F9;
            }
        }
    } else {
        /* Normal Mode: solid block cursor (#6495ed) with inverted character glyph */
        int block_w = 12;
        char cur_char_str[8] = " ";
        if (state->cursor_pos < state->query_len) {
            const char *p = &state->query[state->cursor_pos];
            const char *next_p = p;
            decode_utf8(&next_p);
            int clen = next_p - p;
            if (clen > 0 && clen < 8) {
                memcpy(cur_char_str, p, clen);
                cur_char_str[clen] = '\0';
                block_w = measure_text_width(0, cur_char_str);
                if (block_w < 8) block_w = 8;
            }
        }

        /* Draw block rectangle */
        for (int y = cur_y; y < cur_y + cur_h && y < (int)height; y++) {
            for (int x = cur_x; x < cur_x + block_w && x < (int)width; x++) {
                if (x >= 0 && y >= 0) {
                    blend_color_alpha(&argb_buf[y * width + x], 0xFF6495ED, 255);
                }
            }
        }

        /* Draw inverted character glyph on top */
        if (cur_char_str[0] != ' ' && cur_char_str[0] != '\0') {
            render_text(argb_buf, width, height, 0, cur_char_str, cur_x, baseline_y, 0xFF16181E);
        }
    }

    /* Mode Pill on the right side of the textbox */
    const char *mode_str = (state->mode == VIM_MODE_NORMAL) ? "NORMAL" : "INSERT";
    int pill_text_w = measure_text_width(1, mode_str);
    int pill_w = pill_text_w + 14;
    int pill_h = 24;
    int pill_x = l->textbox_x + l->textbox_w - pill_w - 12;
    int pill_y = l->textbox_y + (l->textbox_h - pill_h) / 2;

    uint32_t pill_bg = (state->mode == VIM_MODE_NORMAL) ? 0xDD4084D6 : 0x883B4252;
    draw_rounded_rect(argb_buf, width, height,
                      pill_x, pill_y, pill_w, pill_h,
                      4, pill_bg, 0, 0);
    render_text(argb_buf, width, height, 1, mode_str, pill_x + 7, pill_y + 17, 0xFFECEFF4);

    /* Error Banner below textbox when a command failed */
    if (state->error_msg[0] != '\0') {
        int err_x = l->textbox_x;
        int err_y = l->textbox_y + l->textbox_h + 6;
        int err_w = l->textbox_w;
        int err_h = 30;

        /* Semi-transparent dark red card with red border */
        draw_rounded_rect(argb_buf, width, height,
                          err_x, err_y, err_w, err_h,
                          6, 0xF2351A20, 0xFFE74C3C, 1);

        /* Format text truncated to fit */
        int max_err_w = err_w - 24;
        char disp_err[256];
        const char *src_err = state->error_msg;
        int dots_w = measure_text_width(1, "...");

        int cur_w = 0;
        const char *p = src_err;
        int last_fit_byte = 0;
        bool truncated = false;

        while (*p) {
            uint32_t cp = decode_utf8(&p);
            CachedGlyph *g = get_glyph(1, cp);
            int adv = g ? g->advance_x : 0;
            if (cur_w + adv + dots_w <= max_err_w) {
                last_fit_byte = p - src_err;
            }
            cur_w += adv;
            if (cur_w > max_err_w) {
                truncated = true;
                break;
            }
        }

        if (truncated && last_fit_byte > 0 && last_fit_byte < 240) {
            memcpy(disp_err, src_err, last_fit_byte);
            disp_err[last_fit_byte] = '\0';
            strcat(disp_err, "...");
        } else {
            strncpy(disp_err, src_err, sizeof(disp_err) - 1);
            disp_err[sizeof(disp_err) - 1] = '\0';
        }

        render_text(argb_buf, width, height, 1, disp_err, err_x + 12, err_y + 20, 0xFFFFB4B4);
    }

    /* 2. APP GRID */
    if (l->cols <= 0 || l->visible_rows <= 0) return;

    for (int r = 0; r < l->visible_rows; r++) {
        for (int c = 0; c < l->cols; c++) {
            int filtered_idx = (state->scroll_row + r) * l->cols + c;
            if (filtered_idx >= (int)state->filtered_count) break;

            int app_idx = state->filtered[filtered_idx];
            if (app_idx < 0 || (size_t)app_idx >= state->app_count) continue;
            const AppEntry *app = &state->apps[app_idx];

            int cell_x = l->grid_x + c * (l->cell_w + l->cell_pad_x);
            int cell_y = l->grid_y + r * (l->cell_h + l->cell_pad_y);

            bool is_selected = (state->focus == FOCUS_GRID && filtered_idx == state->selected_idx);

            /* Tile card background with radius 12 */
            if (is_selected) {
                /* Highlighted card: #4084d6 / #3b6ea8 with subtle border #6495ed, radius 12px */
                draw_rounded_rect(argb_buf, width, height,
                                  cell_x, cell_y, l->cell_w, l->cell_h,
                                  12, 0xE63B6EA8, 0xFF6495ED, 2);
            } else {
                /* Subtle card background: rgba(64, 69, 82, 0.45) -> 0x73404552, radius 12px */
                draw_rounded_rect(argb_buf, width, height,
                                  cell_x, cell_y, l->cell_w, l->cell_h,
                                  12, 0x73404552, 0, 0);
            }

            /* Icon centered horizontally at cell_x + (cell_w - 64)/2, cell_y + 14 */
            int icon_x = cell_x + (l->cell_w - ICON_SIZE) / 2;
            int icon_y = cell_y + 14;
            blit_icon(argb_buf, width, height, app->icon_pixels, icon_x, icon_y);

            /* App Name label: centered horizontally below icon, baseline at cell_y + 104 */
            int max_text_w = l->cell_w - 16;
            char display_name[128];
            const char *src_name = app->name;
            int dots_w = measure_text_width(1, "...");

            /* O(N) single forward pass for label truncation */
            int cur_w = 0;
            const char *p = src_name;
            int last_fit_byte = 0;
            int total_w = 0;
            bool truncated = false;

            while (*p) {
                uint32_t cp = decode_utf8(&p);
                CachedGlyph *g = get_glyph(1, cp);
                int adv = g ? g->advance_x : 0;

                if (cur_w + adv + dots_w <= max_text_w) {
                    last_fit_byte = p - src_name;
                }
                cur_w += adv;
                if (cur_w > max_text_w) {
                    truncated = true;
                    break;
                }
            }
            total_w = cur_w;

            int final_name_w = 0;
            if (truncated && last_fit_byte > 0 && last_fit_byte < 120) {
                memcpy(display_name, src_name, last_fit_byte);
                display_name[last_fit_byte] = '\0';
                strcat(display_name, "...");
                final_name_w = measure_text_width(1, display_name);
            } else if (!truncated) {
                strncpy(display_name, src_name, sizeof(display_name) - 1);
                display_name[sizeof(display_name) - 1] = '\0';
                final_name_w = total_w;
            } else {
                strncpy(display_name, src_name, sizeof(display_name) - 1);
                display_name[sizeof(display_name) - 1] = '\0';
                final_name_w = measure_text_width(1, display_name);
            }

            int label_x = cell_x + (l->cell_w - final_name_w) / 2;
            int label_baseline_y = cell_y + 104;
            uint32_t text_col = is_selected ? 0xFFFFFFFF : 0xFFD8DEE9;
            render_text(argb_buf, width, height, 1, display_name, label_x, label_baseline_y, text_col);
        }
    }
}
