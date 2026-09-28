#include "launcher.h"
#include <ctype.h>
#include <string.h>

static int clamp_val(int v, int min_val, int max_val) {
    if (v < min_val) return min_val;
    if (v > max_val) return max_val;
    return v;
}

/* Helper to get previous UTF-8 codepoint byte index */
static int prev_utf8_char(const char *str, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)str[pos] & 0xC0) == 0x80) {
        pos--;
    }
    return pos;
}

/* Helper to get next UTF-8 codepoint byte index */
static int next_utf8_char(const char *str, int len, int pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)str[pos] & 0xC0) == 0x80) {
        pos++;
    }
    return pos;
}

static void save_undo(LauncherState *state) {
    strncpy(state->undo_query, state->query, MAX_QUERY - 1);
    state->undo_query[MAX_QUERY - 1] = '\0';
    state->undo_cursor = state->cursor_pos;
}

static void restore_undo(LauncherState *state) {
    char tmp_q[MAX_QUERY];
    int tmp_c = state->cursor_pos;
    strncpy(tmp_q, state->query, MAX_QUERY - 1);
    tmp_q[MAX_QUERY - 1] = '\0';

    strncpy(state->query, state->undo_query, MAX_QUERY - 1);
    state->query[MAX_QUERY - 1] = '\0';
    state->query_len = (int)strlen(state->query);
    state->cursor_pos = clamp_val(state->undo_cursor, 0, state->query_len);
    if (state->mode == VIM_MODE_NORMAL && state->query_len > 0 && state->cursor_pos >= state->query_len) {
        state->cursor_pos = state->query_len - 1;
    }

    strncpy(state->undo_query, tmp_q, MAX_QUERY - 1);
    state->undo_query[MAX_QUERY - 1] = '\0';
    state->undo_cursor = tmp_c;

    state->history_idx = -1;
    filter_apps(state);
    state->path_status = check_path_executable(state->query);
}

static void delete_range(LauncherState *state, int start, int end) {
    if (start > end) {
        int tmp = start;
        start = end;
        end = tmp;
    }
    start = clamp_val(start, 0, state->query_len);
    end = clamp_val(end, 0, state->query_len);
    if (start == end) return;

    int yank_len = end - start;
    if (yank_len >= MAX_QUERY) yank_len = MAX_QUERY - 1;
    memcpy(state->yank_buf, &state->query[start], yank_len);
    state->yank_buf[yank_len] = '\0';

    memmove(&state->query[start], &state->query[end], state->query_len - end + 1);
    state->query_len -= (end - start);
    state->cursor_pos = start;
    if (state->mode == VIM_MODE_NORMAL && state->query_len > 0 && state->cursor_pos >= state->query_len) {
        state->cursor_pos = state->query_len - 1;
    }
    state->history_idx = -1;
    filter_apps(state);
    state->path_status = check_path_executable(state->query);
}

/* Character class classification for Vim word motions:
 * 0: whitespace
 * 1: word character (alphanumeric, underscore)
 * 2: punctuation / other non-whitespace
 */
static int char_class(char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
    if (isalnum((unsigned char)c) || c == '_') return 1;
    return 2;
}

/* Vim word motions */
static int motion_w(const char *q, int len, int pos) {
    if (pos >= len) return len;
    int cls = char_class(q[pos]);
    int p = pos;
    if (cls != 0) {
        while (p < len && char_class(q[p]) == cls) p++;
    }
    while (p < len && char_class(q[p]) == 0) p++;
    return p;
}

static int motion_W(const char *q, int len, int pos) {
    if (pos >= len) return len;
    int p = pos;
    if (char_class(q[p]) != 0) {
        while (p < len && char_class(q[p]) != 0) p++;
    }
    while (p < len && char_class(q[p]) == 0) p++;
    return p;
}

static int motion_e(const char *q, int len, int pos) {
    if (pos >= len - 1) return len > 0 ? len - 1 : 0;
    int p = pos + 1;
    while (p < len && char_class(q[p]) == 0) p++;
    if (p >= len) return len - 1;
    int cls = char_class(q[p]);
    while (p + 1 < len && char_class(q[p + 1]) == cls) p++;
    return p;
}

static int motion_E(const char *q, int len, int pos) {
    if (pos >= len - 1) return len > 0 ? len - 1 : 0;
    int p = pos + 1;
    while (p < len && char_class(q[p]) == 0) p++;
    if (p >= len) return len - 1;
    while (p + 1 < len && char_class(q[p + 1]) != 0) p++;
    return p;
}

static int motion_b(const char *q, int len, int pos) {
    (void)len;
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && char_class(q[p]) == 0) p--;
    int cls = char_class(q[p]);
    while (p > 0 && char_class(q[p - 1]) == cls) p--;
    return p;
}

static int motion_B(const char *q, int len, int pos) {
    (void)len;
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && char_class(q[p]) == 0) p--;
    while (p > 0 && char_class(q[p - 1]) != 0) p--;
    return p;
}

static int motion_0(const char *q, int len, int pos) {
    (void)q; (void)len; (void)pos;
    return 0;
}

static int motion_caret(const char *q, int len, int pos) {
    (void)pos;
    int p = 0;
    while (p < len && (q[p] == ' ' || q[p] == '\t')) p++;
    if (p >= len && len > 0) p = len - 1;
    return p;
}

static int motion_dollar(const char *q, int len, int pos) {
    (void)q; (void)pos;
    return len > 0 ? len - 1 : 0;
}

static void update_scroll_to_visible(LauncherState *state) {
    if (state->filtered_count == 0) {
        state->scroll_row = 0;
        state->selected_idx = 0;
        return;
    }
    int cols = state->layout.cols > 0 ? state->layout.cols : 1;
    int vis_rows = state->layout.visible_rows > 0 ? state->layout.visible_rows : 1;

    state->selected_idx = clamp_val(state->selected_idx, 0, (int)state->filtered_count - 1);
    int total_rows = ((int)state->filtered_count + cols - 1) / cols;
    int max_scroll = total_rows - vis_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (state->scroll_row > max_scroll) state->scroll_row = max_scroll;

    int sel_row = state->selected_idx / cols;

    if (sel_row < state->scroll_row) {
        state->scroll_row = sel_row;
    } else if (sel_row >= state->scroll_row + vis_rows) {
        state->scroll_row = sel_row - vis_rows + 1;
    }
    if (state->scroll_row < 0) state->scroll_row = 0;
}

void vim_init(LauncherState *state) {
    state->mode = VIM_MODE_INSERT;
    state->focus = FOCUS_TEXTBOX;
    state->pending_op = 0;
    state->query[0] = '\0';
    state->query_len = 0;
    state->cursor_pos = 0;
    state->undo_query[0] = '\0';
    state->undo_cursor = 0;
    state->yank_buf[0] = '\0';
    state->selected_idx = 0;
    state->scroll_row = 0;
    state->path_status = PATH_STATUS_EMPTY;
    state->error_msg[0] = '\0';
    state->history_count = 0;
    state->history_idx = -1;
    state->history_saved_query[0] = '\0';
}

static bool handle_launch(LauncherState *state, uint32_t mods) {
    if (mods & MOD_SHIFT) {
        if (state->query_len > 0) {
            history_add(state, state->query);
            int rc = launch_raw_command(state->query, state->error_msg, sizeof(state->error_msg));
            if (rc == 0) {
                state->running = false;
            } else {
                state->needs_redraw = true;
            }
            return true;
        }
    }

    if (state->focus == FOCUS_GRID) {
        if (state->selected_idx >= 0 && (size_t)state->selected_idx < state->filtered_count) {
            int app_idx = state->filtered[state->selected_idx];
            cache_record_launch(state, app_idx);
            launch_desktop_app(&state->apps[app_idx]);
            state->running = false;
            return true;
        }
    } else {
        /* FOCUS_TEXTBOX */
        if (state->history_idx >= 0 || state->filtered_count == 0 || state->path_status == PATH_STATUS_VALID) {
            if (state->query_len > 0) {
                history_add(state, state->query);
                int rc = launch_raw_command(state->query, state->error_msg, sizeof(state->error_msg));
                if (rc == 0) {
                    state->running = false;
                } else {
                    state->needs_redraw = true;
                }
                return true;
            }
        } else if (state->filtered_count > 0) {
            int app_idx = state->filtered[0];
            cache_record_launch(state, app_idx);
            launch_desktop_app(&state->apps[app_idx]);
            state->running = false;
            return true;
        }
    }
    return false;
}

static bool handle_nav_key(LauncherState *state, xkb_keysym_t sym, uint32_t mods) {
    int cols = state->layout.cols > 0 ? state->layout.cols : 1;
    int total = (int)state->filtered_count;

    switch (sym) {
        case XKB_KEY_Tab:
            if (state->focus == FOCUS_TEXTBOX) {
                if (total > 0) {
                    state->focus = FOCUS_GRID;
                    state->selected_idx = 0;
                    update_scroll_to_visible(state);
                    return true;
                }
                return false;
            }
            /* In FOCUS_GRID */
            if (mods & MOD_SHIFT) {
                if (state->selected_idx == 0) {
                    state->focus = FOCUS_TEXTBOX;
                    return true;
                } else if (total > 0) {
                    state->selected_idx = (state->selected_idx - 1 + total) % total;
                    update_scroll_to_visible(state);
                    return true;
                }
            } else {
                if (total > 0) {
                    state->selected_idx = (state->selected_idx + 1) % total;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;

        case XKB_KEY_ISO_Left_Tab:
            if (state->focus == FOCUS_GRID) {
                if (state->selected_idx == 0) {
                    state->focus = FOCUS_TEXTBOX;
                    return true;
                } else if (total > 0) {
                    state->selected_idx = (state->selected_idx - 1 + total) % total;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;

        case XKB_KEY_Left:
            if (state->focus == FOCUS_TEXTBOX) {
                state->cursor_pos = prev_utf8_char(state->query, state->cursor_pos);
                return true;
            } else {
                if (total > 0 && state->selected_idx > 0) {
                    state->selected_idx--;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;

        case XKB_KEY_Right:
            if (state->focus == FOCUS_TEXTBOX) {
                state->cursor_pos = next_utf8_char(state->query, state->query_len, state->cursor_pos);
                return true;
            } else {
                if (total > 0 && state->selected_idx + 1 < total) {
                    state->selected_idx++;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;

        case XKB_KEY_Up:
            if (state->focus == FOCUS_TEXTBOX) {
                if (state->history_count > 0) {
                    if (state->history_idx == -1) {
                        strncpy(state->history_saved_query, state->query, sizeof(state->history_saved_query) - 1);
                        state->history_saved_query[sizeof(state->history_saved_query) - 1] = '\0';
                        state->history_idx = (int)state->history_count - 1;
                    } else if (state->history_idx > 0) {
                        state->history_idx--;
                    }
                    strncpy(state->query, state->history[state->history_idx], sizeof(state->query) - 1);
                    state->query[sizeof(state->query) - 1] = '\0';
                    state->query_len = (int)strlen(state->query);
                    state->cursor_pos = state->query_len;
                    filter_apps(state);
                    state->path_status = check_path_executable(state->query);
                    return true;
                }
                return false;
            } else {
                int cur_row = state->selected_idx / cols;
                if (cur_row == 0) {
                    state->focus = FOCUS_TEXTBOX;
                    return true;
                } else {
                    state->selected_idx -= cols;
                    if (state->selected_idx < 0) state->selected_idx = 0;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;

        case XKB_KEY_Down:
            if (state->focus == FOCUS_TEXTBOX) {
                if (state->history_idx >= 0) {
                    if (state->history_idx + 1 < (int)state->history_count) {
                        state->history_idx++;
                        strncpy(state->query, state->history[state->history_idx], sizeof(state->query) - 1);
                        state->query[sizeof(state->query) - 1] = '\0';
                        state->query_len = (int)strlen(state->query);
                        state->cursor_pos = state->query_len;
                    } else {
                        state->history_idx = -1;
                        strncpy(state->query, state->history_saved_query, sizeof(state->query) - 1);
                        state->query[sizeof(state->query) - 1] = '\0';
                        state->query_len = (int)strlen(state->query);
                        state->cursor_pos = state->query_len;
                    }
                    filter_apps(state);
                    state->path_status = check_path_executable(state->query);
                    return true;
                } else {
                    if (total > 0) {
                        state->focus = FOCUS_GRID;
                        state->selected_idx = 0;
                        update_scroll_to_visible(state);
                        return true;
                    }
                }
                return false;
            } else {
                if (total > 0 && state->selected_idx + cols < total) {
                    state->selected_idx += cols;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;

        default:
            return false;
    }
}

static bool is_modifier_keysym(xkb_keysym_t sym) {
    return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) ||
           sym == XKB_KEY_Mode_switch || sym == XKB_KEY_ISO_Level3_Shift;
}

static bool vim_handle_key_inner(LauncherState *state, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        return handle_launch(state, mods);
    }

    if (sym == XKB_KEY_Left || sym == XKB_KEY_Right ||
        sym == XKB_KEY_Up || sym == XKB_KEY_Down ||
        sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
        state->pending_op = 0;
        return handle_nav_key(state, sym, mods);
    }

    if ((mods & MOD_CTRL) && (sym == XKB_KEY_p || sym == XKB_KEY_P)) {
        state->pending_op = 0;
        return handle_nav_key(state, XKB_KEY_Up, mods);
    }

    if ((mods & MOD_CTRL) && (sym == XKB_KEY_n || sym == XKB_KEY_N)) {
        state->pending_op = 0;
        return handle_nav_key(state, XKB_KEY_Down, mods);
    }

    /* INSERT MODE */
    if (state->mode == VIM_MODE_INSERT) {
        if (sym == XKB_KEY_Escape ||
            ((mods & MOD_CTRL) && (sym == XKB_KEY_bracketleft || sym == XKB_KEY_c || sym == XKB_KEY_C))) {
            if (state->query_len == 0) {
                state->running = false;
                return true;
            }
            state->mode = VIM_MODE_NORMAL;
            state->pending_op = 0;
            state->cursor_pos = clamp_val(state->cursor_pos, 0, state->query_len - 1);
            return true;
        }

        if (sym == XKB_KEY_BackSpace || ((mods & MOD_CTRL) && (sym == XKB_KEY_h || sym == XKB_KEY_H))) {
            if (state->cursor_pos > 0) {
                save_undo(state);
                int p = prev_utf8_char(state->query, state->cursor_pos);
                int num = state->cursor_pos - p;
                memmove(&state->query[p], &state->query[state->cursor_pos], state->query_len - state->cursor_pos + 1);
                state->query_len -= num;
                state->cursor_pos = p;
                state->focus = FOCUS_TEXTBOX;
                state->history_idx = -1;
                filter_apps(state);
                state->path_status = check_path_executable(state->query);
                return true;
            }
            return false;
        }

        if (sym == XKB_KEY_Delete) {
            if (state->cursor_pos < state->query_len) {
                save_undo(state);
                int next = next_utf8_char(state->query, state->query_len, state->cursor_pos);
                int num = next - state->cursor_pos;
                memmove(&state->query[state->cursor_pos], &state->query[next], state->query_len - next + 1);
                state->query_len -= num;
                state->focus = FOCUS_TEXTBOX;
                state->history_idx = -1;
                filter_apps(state);
                state->path_status = check_path_executable(state->query);
                return true;
            }
            return false;
        }

        if ((mods & MOD_CTRL) && (sym == XKB_KEY_w || sym == XKB_KEY_W)) {
            if (state->cursor_pos > 0) {
                save_undo(state);
                int p = motion_b(state->query, state->query_len, state->cursor_pos);
                delete_range(state, p, state->cursor_pos);
                state->focus = FOCUS_TEXTBOX;
                state->history_idx = -1;
                return true;
            }
            return false;
        }

        if ((mods & MOD_CTRL) && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
            if (state->cursor_pos > 0) {
                save_undo(state);
                delete_range(state, 0, state->cursor_pos);
                state->focus = FOCUS_TEXTBOX;
                state->history_idx = -1;
                return true;
            }
            return false;
        }

        if (sym == XKB_KEY_Home || ((mods & MOD_CTRL) && (sym == XKB_KEY_a || sym == XKB_KEY_A))) {
            state->cursor_pos = 0;
            return true;
        }

        if (sym == XKB_KEY_End || ((mods & MOD_CTRL) && (sym == XKB_KEY_e || sym == XKB_KEY_E))) {
            state->cursor_pos = state->query_len;
            return true;
        }

        if ((mods & MOD_CTRL) && sym == XKB_KEY_j) {
            int cols = state->layout.cols > 0 ? state->layout.cols : 1;
            int total = (int)state->filtered_count;
            if (state->focus == FOCUS_TEXTBOX) {
                state->focus = FOCUS_GRID;
                state->selected_idx = 0;
                update_scroll_to_visible(state);
                return true;
            } else if (total > 0 && state->selected_idx + cols < total) {
                state->selected_idx += cols;
                update_scroll_to_visible(state);
                return true;
            }
            return false;
        }

        if ((mods & MOD_CTRL) && sym == XKB_KEY_k) {
            int cols = state->layout.cols > 0 ? state->layout.cols : 1;
            if (state->focus == FOCUS_GRID) {
                if (state->selected_idx / cols == 0) {
                    state->focus = FOCUS_TEXTBOX;
                } else {
                    state->selected_idx -= cols;
                    if (state->selected_idx < 0) state->selected_idx = 0;
                }
                update_scroll_to_visible(state);
                return true;
            }
            return false;
        }

        /* Printable text insertion */
        if (!(mods & MOD_CTRL) && !(mods & MOD_ALT) && utf8 && utf8[0] != '\0') {
            size_t ulen = strlen(utf8);
            if (ulen > 0 && (unsigned char)utf8[0] >= 32) {
                if (state->query_len + (int)ulen < MAX_QUERY) {
                    save_undo(state);
                    memmove(&state->query[state->cursor_pos + ulen],
                            &state->query[state->cursor_pos],
                            state->query_len - state->cursor_pos + 1);
                    memcpy(&state->query[state->cursor_pos], utf8, ulen);
                    state->query_len += (int)ulen;
                    state->cursor_pos += (int)ulen;
                    state->query[state->query_len] = '\0';
                    state->focus = FOCUS_TEXTBOX;
                    state->history_idx = -1;
                    filter_apps(state);
                    state->path_status = check_path_executable(state->query);
                    return true;
                }
            }
        }
        return false;
    }

    /* NORMAL MODE */
    if (sym == XKB_KEY_Escape) {
        if (state->pending_op != 0) {
            state->pending_op = 0;
            return true;
        }
        state->running = false;
        return true;
    }

    /* Undo */
    if (sym == XKB_KEY_u && !(mods & (MOD_CTRL | MOD_ALT))) {
        state->pending_op = 0;
        restore_undo(state);
        return true;
    }

    /* gg jump */
    if (sym == XKB_KEY_g && !(mods & (MOD_CTRL | MOD_ALT))) {
        if (state->pending_op == 'g') {
            state->pending_op = 0;
            state->selected_idx = 0;
            state->scroll_row = 0;
            state->focus = FOCUS_GRID;
            return true;
        } else {
            state->pending_op = 'g';
            return true;
        }
    }

    /* G jump */
    if ((sym == XKB_KEY_G || (sym == XKB_KEY_g && (mods & MOD_SHIFT))) && !(mods & (MOD_CTRL | MOD_ALT))) {
        state->pending_op = 0;
        if (state->filtered_count > 0) {
            state->selected_idx = (int)state->filtered_count - 1;
            state->focus = FOCUS_GRID;
            update_scroll_to_visible(state);
        }
        return true;
    }

    /* Pending 'd' or 'c' operator */
    if (state->pending_op == 'd' || state->pending_op == 'c') {
        bool is_change = (state->pending_op == 'c');
        char op = state->pending_op;
        state->pending_op = 0;

        /* Doubled operator: 'dd' or 'cc' */
        if ((op == 'd' && sym == XKB_KEY_d) || (op == 'c' && sym == XKB_KEY_c)) {
            save_undo(state);
            strncpy(state->yank_buf, state->query, MAX_QUERY - 1);
            state->yank_buf[MAX_QUERY - 1] = '\0';
            state->query[0] = '\0';
            state->query_len = 0;
            state->cursor_pos = 0;
            if (is_change) {
                state->mode = VIM_MODE_INSERT;
            }
            state->focus = FOCUS_GRID;
            filter_apps(state);
            state->path_status = check_path_executable(state->query);
            return true;
        }

        int target = -1;
        bool inclusive = false;
        if (sym == XKB_KEY_w) { target = motion_w(state->query, state->query_len, state->cursor_pos); }
        else if (sym == XKB_KEY_W) { target = motion_W(state->query, state->query_len, state->cursor_pos); }
        else if (sym == XKB_KEY_b) { target = motion_b(state->query, state->query_len, state->cursor_pos); }
        else if (sym == XKB_KEY_B) { target = motion_B(state->query, state->query_len, state->cursor_pos); }
        else if (sym == XKB_KEY_e) { target = motion_e(state->query, state->query_len, state->cursor_pos); inclusive = true; }
        else if (sym == XKB_KEY_E) { target = motion_E(state->query, state->query_len, state->cursor_pos); inclusive = true; }
        else if (sym == XKB_KEY_0) { target = motion_0(state->query, state->query_len, state->cursor_pos); }
        else if (sym == XKB_KEY_asciicircum) { target = motion_caret(state->query, state->query_len, state->cursor_pos); }
        else if (sym == XKB_KEY_dollar) { target = motion_dollar(state->query, state->query_len, state->cursor_pos); inclusive = true; }
        else if (sym == XKB_KEY_h) { target = prev_utf8_char(state->query, state->cursor_pos); }
        else if (sym == XKB_KEY_l) { target = next_utf8_char(state->query, state->query_len, state->cursor_pos); inclusive = true; }

        if (target >= 0) {
            save_undo(state);
            int start = state->cursor_pos;
            int end = target;
            if (start > end) {
                int tmp = start; start = end; end = tmp;
            } else if (inclusive && end < state->query_len) {
                end = next_utf8_char(state->query, state->query_len, end);
            }
            delete_range(state, start, end);
            if (is_change) {
                state->mode = VIM_MODE_INSERT;
                state->focus = FOCUS_GRID;
            }
            return true;
        }
        return false;
    }

    state->pending_op = 0;

    /* Single key normal mode commands */
    switch (sym) {
        case XKB_KEY_d:
            state->pending_op = 'd';
            return true;

        case XKB_KEY_c:
            state->pending_op = 'c';
            return true;

        case XKB_KEY_D:
            save_undo(state);
            delete_range(state, state->cursor_pos, state->query_len);
            return true;

        case XKB_KEY_S:
            save_undo(state);
            strncpy(state->yank_buf, state->query, MAX_QUERY - 1);
            state->yank_buf[MAX_QUERY - 1] = '\0';
            state->query[0] = '\0';
            state->query_len = 0;
            state->cursor_pos = 0;
            state->mode = VIM_MODE_INSERT;
            state->focus = FOCUS_GRID;
            filter_apps(state);
            state->path_status = check_path_executable(state->query);
            return true;

        case XKB_KEY_C:
            save_undo(state);
            delete_range(state, state->cursor_pos, state->query_len);
            state->mode = VIM_MODE_INSERT;
            state->focus = FOCUS_GRID;
            return true;

        case XKB_KEY_x:
            if (state->cursor_pos < state->query_len) {
                save_undo(state);
                int next = next_utf8_char(state->query, state->query_len, state->cursor_pos);
                delete_range(state, state->cursor_pos, next);
                return true;
            }
            return false;

        case XKB_KEY_X:
            if (state->cursor_pos > 0) {
                save_undo(state);
                int prev = prev_utf8_char(state->query, state->cursor_pos);
                delete_range(state, prev, state->cursor_pos);
                return true;
            }
            return false;

        case XKB_KEY_s:
            if (state->cursor_pos < state->query_len) {
                save_undo(state);
                int next = next_utf8_char(state->query, state->query_len, state->cursor_pos);
                delete_range(state, state->cursor_pos, next);
            }
            state->mode = VIM_MODE_INSERT;
            state->focus = FOCUS_GRID;
            return true;

        case XKB_KEY_p: {
            size_t ylen = strlen(state->yank_buf);
            if (ylen > 0) {
                save_undo(state);
                int ins_pos = state->query_len > 0 ? next_utf8_char(state->query, state->query_len, state->cursor_pos) : 0;
                if (ins_pos > state->query_len) ins_pos = state->query_len;
                if (state->query_len + (int)ylen < MAX_QUERY) {
                    memmove(&state->query[ins_pos + ylen], &state->query[ins_pos], state->query_len - ins_pos + 1);
                    memcpy(&state->query[ins_pos], state->yank_buf, ylen);
                    state->query_len += (int)ylen;
                    state->cursor_pos = ins_pos + (int)ylen - 1;
                    state->focus = FOCUS_GRID;
                    filter_apps(state);
                    state->path_status = check_path_executable(state->query);
                    return true;
                }
            }
            return false;
        }

        case XKB_KEY_P: {
            size_t ylen = strlen(state->yank_buf);
            if (ylen > 0) {
                save_undo(state);
                int ins_pos = state->cursor_pos;
                if (ins_pos > state->query_len) ins_pos = state->query_len;
                if (state->query_len + (int)ylen < MAX_QUERY) {
                    memmove(&state->query[ins_pos + ylen], &state->query[ins_pos], state->query_len - ins_pos + 1);
                    memcpy(&state->query[ins_pos], state->yank_buf, ylen);
                    state->query_len += (int)ylen;
                    state->cursor_pos = ins_pos + (int)ylen - 1;
                    state->focus = FOCUS_GRID;
                    filter_apps(state);
                    state->path_status = check_path_executable(state->query);
                    return true;
                }
            }
            return false;
        }

        case XKB_KEY_i:
            state->mode = VIM_MODE_INSERT;
            state->focus = FOCUS_GRID;
            return true;

        case XKB_KEY_a:
            state->mode = VIM_MODE_INSERT;
            state->focus = FOCUS_GRID;
            if (state->query_len > 0) {
                state->cursor_pos = next_utf8_char(state->query, state->query_len, state->cursor_pos);
            }
            return true;

        case XKB_KEY_I:
            state->mode = VIM_MODE_INSERT;
            state->focus = FOCUS_GRID;
            state->cursor_pos = motion_caret(state->query, state->query_len, state->cursor_pos);
            return true;

        case XKB_KEY_A:
            state->mode = VIM_MODE_INSERT;
            state->focus = FOCUS_GRID;
            state->cursor_pos = state->query_len;
            return true;

        /* Motions */
        case XKB_KEY_w:
            state->cursor_pos = motion_w(state->query, state->query_len, state->cursor_pos);
            if (state->cursor_pos >= state->query_len && state->query_len > 0)
                state->cursor_pos = state->query_len - 1;
            return true;

        case XKB_KEY_W:
            state->cursor_pos = motion_W(state->query, state->query_len, state->cursor_pos);
            if (state->cursor_pos >= state->query_len && state->query_len > 0)
                state->cursor_pos = state->query_len - 1;
            return true;

        case XKB_KEY_b:
            state->cursor_pos = motion_b(state->query, state->query_len, state->cursor_pos);
            return true;

        case XKB_KEY_B:
            state->cursor_pos = motion_B(state->query, state->query_len, state->cursor_pos);
            return true;

        case XKB_KEY_e:
            state->cursor_pos = motion_e(state->query, state->query_len, state->cursor_pos);
            return true;

        case XKB_KEY_E:
            state->cursor_pos = motion_E(state->query, state->query_len, state->cursor_pos);
            return true;

        case XKB_KEY_0:
            state->cursor_pos = 0;
            return true;

        case XKB_KEY_asciicircum:
            state->cursor_pos = motion_caret(state->query, state->query_len, state->cursor_pos);
            return true;

        case XKB_KEY_dollar:
            state->cursor_pos = motion_dollar(state->query, state->query_len, state->cursor_pos);
            return true;

        /* Grid / cursor navigation */
        case XKB_KEY_j: {
            int cols = state->layout.cols > 0 ? state->layout.cols : 1;
            int total = (int)state->filtered_count;
            if (state->focus == FOCUS_TEXTBOX) {
                state->focus = FOCUS_GRID;
                update_scroll_to_visible(state);
                return true;
            } else {
                if (total > 0 && state->selected_idx + cols < total) {
                    state->selected_idx += cols;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;
        }

        case XKB_KEY_k: {
            int cols = state->layout.cols > 0 ? state->layout.cols : 1;
            if (state->focus == FOCUS_GRID) {
                if (state->selected_idx / cols == 0) {
                    state->focus = FOCUS_TEXTBOX;
                } else {
                    state->selected_idx -= cols;
                    if (state->selected_idx < 0) state->selected_idx = 0;
                }
                update_scroll_to_visible(state);
                return true;
            }
            return false;
        }

        case XKB_KEY_h:
            if (state->focus == FOCUS_TEXTBOX) {
                state->cursor_pos = prev_utf8_char(state->query, state->cursor_pos);
                return true;
            } else {
                if (state->selected_idx > 0) {
                    state->selected_idx--;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;

        case XKB_KEY_l:
            if (state->focus == FOCUS_TEXTBOX) {
                if (state->cursor_pos + 1 < state->query_len) {
                    state->cursor_pos = next_utf8_char(state->query, state->query_len, state->cursor_pos);
                    return true;
                }
            } else {
                if (state->selected_idx + 1 < (int)state->filtered_count) {
                    state->selected_idx++;
                    update_scroll_to_visible(state);
                    return true;
                }
            }
            return false;

        default:
            break;
    }

    return false;
}

bool vim_handle_key(LauncherState *state, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
    bool had_error = false;
    if (!is_modifier_keysym(sym) && state->error_msg[0] != '\0') {
        state->error_msg[0] = '\0';
        had_error = true;
    }
    bool handled = vim_handle_key_inner(state, sym, utf8, mods);
    return handled || had_error;
}
