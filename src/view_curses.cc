/**
 * Copyright (c) 2007-2012, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, 'OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file view_curses.cc
 */

#include <chrono>
#include <cmath>
#include <iterator>
#include <string>

#include "view_curses.hh"

#include <zlib.h>

#include "base/ansi_scrubber.hh"
#include "base/attr_line.hh"
#include "base/from_trait.hh"
#include "base/injector.hh"
#include "base/itertools.enumerate.hh"
#include "base/itertools.hh"
#include "base/lnav_log.hh"
#include "config.h"
#include "lnav_config.hh"
#include "shlex.hh"
#include "terminfo-files.h"
#include "terminfo/terminfo.h"
#include "uniwidth.h"
#include "xterm_mouse.hh"

using namespace std::chrono_literals;

const struct itimerval ui_periodic_timer::INTERVAL = {
    {0, std::chrono::duration_cast<std::chrono::microseconds>(100ms).count()},
    {0, std::chrono::duration_cast<std::chrono::microseconds>(100ms).count()},
};

ui_periodic_timer::ui_periodic_timer()
{
    struct sigaction sa;

    if (getenv("lnav_test") != nullptr) {
        this->upt_deadline = std::chrono::steady_clock::now() + 5s;
    }

    sa.sa_handler = ui_periodic_timer::sigalrm;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, nullptr);
    if (setitimer(ITIMER_REAL, &INTERVAL, nullptr) == -1) {
        perror("setitimer");
    }
}

ui_periodic_timer&
ui_periodic_timer::singleton()
{
    static ui_periodic_timer retval;

    return retval;
}

void
ui_periodic_timer::sigalrm(int sig)
{
    auto& upt = singleton();

    if (upt.upt_deadline
        && std::chrono::steady_clock::now() > upt.upt_deadline.value())
    {
        abort();
    }
    upt.upt_counter += 1;
}

alerter&
alerter::singleton()
{
    static alerter retval;

    return retval;
}

bool
alerter::chime(std::string msg)
{
    if (!this->a_enabled) {
        return true;
    }

    bool retval = this->a_do_flash;
    if (this->a_do_flash) {
        static const auto BELL = "\a";
        log_warning("chime message: %s", msg.c_str());
        write(STDIN_FILENO, BELL, 1);
    }
    this->a_do_flash = false;
    return retval;
}

struct utf_to_display_adjustment {
    int uda_origin;
    int uda_offset;

    utf_to_display_adjustment(int utf_origin, int offset)
        : uda_origin(utf_origin), uda_offset(offset)
    {
    }
};

bool
mouse_event::is_click(mouse_button_t button) const
{
    return this->me_button == button
        && this->me_state == mouse_button_state_t::BUTTON_STATE_RELEASED
        && this->me_press_x == this->me_x && this->me_press_y == this->me_y;
}

bool
mouse_event::is_click_in(mouse_button_t button, int x_start, int x_end) const
{
    return this->me_button == button
        && this->me_state == mouse_button_state_t::BUTTON_STATE_RELEASED
        && (x_start <= this->me_x && this->me_x <= x_end)
        && (x_start <= this->me_press_x && this->me_press_x <= x_end)
        && this->me_y == this->me_press_y;
}

bool
mouse_event::is_press_in(mouse_button_t button, line_range lr) const
{
    return this->me_button == button
        && this->me_state == mouse_button_state_t::BUTTON_STATE_PRESSED
        && lr.contains(this->me_x);
}

bool
mouse_event::is_drag_in(mouse_button_t button, line_range lr) const
{
    return this->me_button == button
        && this->me_state == mouse_button_state_t::BUTTON_STATE_DRAGGED
        && lr.contains(this->me_press_x) && lr.contains(this->me_x);
}

bool
mouse_event::is_double_click_in(mouse_button_t button, line_range lr) const
{
    return this->me_button == button
        && this->me_state == mouse_button_state_t::BUTTON_STATE_DOUBLE_CLICK
        && lr.contains(this->me_x) && this->me_y == this->me_press_y;
}

bool
view_curses::do_update()
{
    bool retval = false;

    this->vc_needs_update = false;

    if (!this->vc_visible) {
        return retval;
    }

    for (auto* child : this->vc_children) {
        retval = child->do_update() || retval;
    }
    return retval;
}

bool
view_curses::handle_mouse(mouse_event& me)
{
    if (me.me_state != mouse_button_state_t::BUTTON_STATE_DRAGGED) {
        this->vc_last_drag_child = nullptr;
    }

    for (auto* child : this->vc_children) {
        auto x = this->vc_x + me.me_x;
        auto y = this->vc_y + me.me_y;
        if ((me.me_state == mouse_button_state_t::BUTTON_STATE_DRAGGED
             && child == this->vc_last_drag_child && child->vc_x <= x
             && x < (child->vc_x + child->vc_width))
            || child->contains(x, y))
        {
            auto sub_me = me;

            sub_me.me_x = x - child->vc_x;
            sub_me.me_y = y - child->vc_y;
            sub_me.me_press_x = this->vc_x + me.me_press_x - child->vc_x;
            sub_me.me_press_y = this->vc_y + me.me_press_y - child->vc_y;
            if (me.me_state == mouse_button_state_t::BUTTON_STATE_DRAGGED) {
                this->vc_last_drag_child = child;
            }
            return child->handle_mouse(sub_me);
        }
    }
    return false;
}

std::optional<view_curses*>
view_curses::contains(int x, int y)
{
    if (!this->vc_visible || !this->vc_enabled) {
        return std::nullopt;
    }

    for (auto* child : this->vc_children) {
        auto contains_res = child->contains(x, y);
        if (contains_res) {
            return contains_res;
        }
    }
    if (this->vc_x <= x
        && (this->vc_width < 0 || x < this->vc_x + this->vc_width)
        && this->vc_y == y)
    {
        return this;
    }
    return std::nullopt;
}

void
view_curses::awaiting_user_input()
{
    static const bool enabled = getenv("IN_SCRIPTY") != nullptr;
    static const char OSC_INPUT[] = "\x1b]999;send-input\a";

    if (enabled) {
        write(STDOUT_FILENO, OSC_INPUT, sizeof(OSC_INPUT) - 1);
    }
}

view_curses::mvwattrline_result
view_curses::mvwattrline(ncplane* window,
                         int y,
                         const int x,
                         attr_line_t& al,
                         const struct line_range& lr_chars,
                         role_t base_role)
{
    auto& sa = al.get_attrs();
    const auto& line = al.get_string();
    std::vector<utf_to_display_adjustment> utf_adjustments;

    require(lr_chars.lr_end >= 0);

    mvwattrline_result retval;
    auto line_width_chars = lr_chars.length();
    std::string expanded_line;
    line_range lr_bytes;
    int char_index = 0;

    {
        unsigned rows, cols;
        ncplane_dim_yx(window, &rows, &cols);

        if (y < 0 || y >= rows || x < 0 || x >= cols) {
            line_width_chars = 0;
        } else if ((x + line_width_chars) > cols) {
            line_width_chars = cols - x;
        }
    }

    for (size_t lpc = 0; lpc < line.size();) {
        int exp_start_index = expanded_line.size();
        auto ch = static_cast<unsigned char>(line[lpc]);

        if (char_index == lr_chars.lr_start) {
            lr_bytes.lr_start = exp_start_index;
        } else if (char_index == lr_chars.lr_end) {
            lr_bytes.lr_end = exp_start_index;
            retval.mr_chars_out = char_index;
        }

        switch (ch) {
            case '\t': {
                do {
                    expanded_line.push_back(' ');
                    char_index += 1;
                    if (char_index == lr_chars.lr_start) {
                        lr_bytes.lr_start = expanded_line.size();
                    }
                    if (char_index == lr_chars.lr_end) {
                        lr_bytes.lr_end = expanded_line.size();
                        retval.mr_chars_out = char_index;
                    }
                } while (expanded_line.size() % 8);
                utf_adjustments.emplace_back(
                    lpc, expanded_line.size() - exp_start_index - 1);
                lpc += 1;
                break;
            }

            case '\x1b':
                expanded_line.append("\u238b");
                utf_adjustments.emplace_back(lpc, -1);
                char_index += 1;
                lpc += 1;
                break;

            case '\b':
                expanded_line.append("\u232b");
                utf_adjustments.emplace_back(lpc, -1);
                char_index += 1;
                lpc += 1;
                break;

            case '\x07':
                expanded_line.append("\U0001F514");
                utf_adjustments.emplace_back(lpc, -1);
                char_index += 1;
                lpc += 1;
                break;

            case '\r':
            case '\n':
                expanded_line.push_back(' ');
                char_index += 1;
                lpc += 1;
                break;

            default: {
                if (ch <= 0x1f) {
                    expanded_line.push_back(0xe2);
                    expanded_line.push_back(0x90);
                    expanded_line.push_back(0x80 + ch);
                    char_index += 1;
                    lpc += 1;
                    break;
                }

                auto exp_read_start = expanded_line.size();
                auto lpc_start = lpc;
                auto read_res
                    = ww898::utf::utf8::read([&line, &expanded_line, &lpc] {
                          auto ch = line[lpc++];
                          expanded_line.push_back(ch);
                          return ch;
                      });

                if (read_res.isErr()) {
                    log_trace(
                        "error:%d:%d:%s", y, x + lpc, read_res.unwrapErr());
                    expanded_line.resize(exp_read_start);
                    expanded_line.push_back('?');
                    char_index += 1;
                    lpc = lpc_start + 1;
                } else {
                    auto wch = read_res.unwrap();
                    auto wcw_res = uc_width(wch, "UTF-8");
                    if (wcw_res < 0) {
                        log_trace(
                            "uc_width(%x) does not recognize width character",
                            wch);
                        wcw_res = 1;
                    }
                    if (lpc > (lpc_start + 1)) {
                        utf_adjustments.emplace_back(
                            lpc_start, wcw_res - (lpc - lpc_start));
                    }
                    char_index += wcw_res;
                    if (lr_bytes.lr_end == -1 && char_index > lr_chars.lr_end) {
                        lr_bytes.lr_end = exp_start_index;
                        retval.mr_chars_out = char_index - wcw_res;
                    }
                }
                break;
            }
        }
    }
    if (lr_bytes.lr_start == -1) {
        lr_bytes.lr_start = expanded_line.size();
    }
    if (lr_bytes.lr_end == -1) {
        lr_bytes.lr_end = expanded_line.size();
    }
    if (retval.mr_chars_out == 0) {
        retval.mr_chars_out = char_index;
    }
    retval.mr_bytes_remaining = expanded_line.size() - lr_bytes.lr_end;
    expanded_line.resize(lr_bytes.lr_end);

    auto& vc = view_colors::singleton();
    auto base_attrs = vc.attrs_for_role(base_role);
    if (lr_chars.length() > 0) {
        ncplane_erase_region(window, y, x, 1, lr_chars.length());
        if (lr_bytes.lr_start < (int) expanded_line.size()) {
            ncplane_putstr_yx(
                window, y, x, &expanded_line.c_str()[lr_bytes.lr_start]);
        } else {
            // Need to move the cursor so the hline call below goes to the
            // right place
            ncplane_cursor_move_yx(window, y, x);
        }
        nccell clear_cell;
        nccell_init(&clear_cell);
        nccell_prime(
            window, &clear_cell, " ", 0, view_colors::to_channels(base_attrs));
        ncplane_hline(
            window, &clear_cell, lr_chars.length() - retval.mr_chars_out);
    }

    text_attrs resolved_line_attrs[line_width_chars + 1];

    std::stable_sort(sa.begin(), sa.end());
    for (auto iter = sa.cbegin(); iter != sa.cend(); ++iter) {
        auto attr_range = iter->sa_range;

        require(attr_range.lr_start >= 0);
        require(attr_range.lr_end >= -1);

        if (!(iter->sa_type == &VC_ROLE || iter->sa_type == &VC_ROLE_FG
              || iter->sa_type == &VC_STYLE || iter->sa_type == &VC_GRAPHIC
              || iter->sa_type == &SA_LEVEL || iter->sa_type == &VC_FOREGROUND
              || iter->sa_type == &VC_BACKGROUND
              || iter->sa_type == &VC_BLOCK_ELEM || iter->sa_type == &VC_ICON))
        {
            continue;
        }

        if (attr_range.lr_unit == line_range::unit::bytes) {
            for (const auto& adj : utf_adjustments) {
                // If the UTF adjustment is in the viewport, we need to adjust
                // this attribute.
                if (adj.uda_origin < iter->sa_range.lr_start) {
                    attr_range.lr_start += adj.uda_offset;
                }
            }

            if (attr_range.lr_end != -1) {
                for (const auto& adj : utf_adjustments) {
                    if (adj.uda_origin < iter->sa_range.lr_end) {
                        attr_range.lr_end += adj.uda_offset;
                    }
                }
            }
        }

        if (attr_range.lr_end == -1) {
            attr_range.lr_end = lr_chars.lr_start + line_width_chars;
        }
        if (attr_range.lr_end < lr_chars.lr_start) {
            continue;
        }
        attr_range.lr_start
            = std::max(0, attr_range.lr_start - lr_chars.lr_start);
        if (attr_range.lr_start > line_width_chars) {
            continue;
        }

        attr_range.lr_end
            = std::min(line_width_chars, attr_range.lr_end - lr_chars.lr_start);

        if (iter->sa_type == &VC_FOREGROUND) {
            auto attr_fg = iter->sa_value.get<styling::color_unit>();
            for (auto lpc = attr_range.lr_start; lpc < attr_range.lr_end; ++lpc)
            {
                resolved_line_attrs[lpc].ta_fg_color = attr_fg;
            }
            continue;
        }

        if (iter->sa_type == &VC_BACKGROUND) {
            auto attr_bg = iter->sa_value.get<styling::color_unit>();
            for (auto lpc = attr_range.lr_start; lpc < attr_range.lr_end; ++lpc)
            {
                resolved_line_attrs[lpc].ta_bg_color = attr_bg;
            }
            continue;
        }

        if (attr_range.lr_start < attr_range.lr_end) {
            auto attrs = text_attrs{};
            std::optional<const char*> graphic;

            if (iter->sa_type == &VC_GRAPHIC) {
                graphic = iter->sa_value.get<const char*>();
                attrs = text_attrs::with_altcharset();
                for (auto lpc = attr_range.lr_start; lpc < attr_range.lr_end;
                     ++lpc)
                {
                    ncplane_putstr_yx(window, y, x + lpc, graphic.value());
                }
            } else if (iter->sa_type == &VC_BLOCK_ELEM) {
                auto be = iter->sa_value.get<block_elem_t>();
                ncplane_pututf32_yx(
                    window, y, x + attr_range.lr_start, be.value);
                attrs = vc.attrs_for_role(be.role);
            } else if (iter->sa_type == &VC_ICON) {
                auto ic = iter->sa_value.get<ui_icon_t>();
                auto be = vc.wchar_for_icon(ic);

                ncplane_pututf32_yx(
                    window, y, x + attr_range.lr_start, be.value);
                attrs = vc.attrs_for_role(be.role);
                // clear the BG color, it interferes with the cursor BG
                attrs.ta_bg_color = styling::color_unit::EMPTY;
            } else if (iter->sa_type == &VC_STYLE) {
                attrs = iter->sa_value.get<text_attrs>();
            } else if (iter->sa_type == &SA_LEVEL) {
                attrs = vc.attrs_for_level(
                    (log_level_t) iter->sa_value.get<int64_t>());
            } else if (iter->sa_type == &VC_ROLE) {
                auto role = iter->sa_value.get<role_t>();
                attrs = vc.attrs_for_role(role);

                if (role == role_t::VCR_SELECTED_TEXT) {
                    retval.mr_selected_text
                        = string_fragment::from_str(line).sub_range(
                            iter->sa_range.lr_start, iter->sa_range.lr_end);
                }
            } else if (iter->sa_type == &VC_ROLE_FG) {
                auto role_attrs
                    = vc.attrs_for_role(iter->sa_value.get<role_t>());
                attrs.ta_fg_color = role_attrs.ta_fg_color;
            }

            if (graphic || !attrs.empty()) {
                if (attrs.ta_fg_color.cu_value.is<styling::semantic>()) {
                    attrs.ta_fg_color
                        = vc.color_for_ident(al.to_string_fragment(iter));
                }
                if (attrs.ta_bg_color.cu_value.is<styling::semantic>()) {
                    attrs.ta_bg_color
                        = vc.color_for_ident(al.to_string_fragment(iter));
                }

                for (auto lpc = attr_range.lr_start; lpc < attr_range.lr_end;
                     ++lpc)
                {
                    auto clear_rev = attrs.has_style(text_attrs::style::reverse)
                        && resolved_line_attrs[lpc].has_style(
                            text_attrs::style::reverse);
                    resolved_line_attrs[lpc] = attrs | resolved_line_attrs[lpc];
                    if (clear_rev) {
                        resolved_line_attrs[lpc].clear_style(
                            text_attrs::style::reverse);
                    }
                }
            }
        }
    }

    for (int lpc = 0; lpc < line_width_chars; lpc++) {
        auto cell_attrs = resolved_line_attrs[lpc] | base_attrs;

        cell_attrs.ta_fg_color = vc.ansi_to_theme_color(cell_attrs.ta_fg_color);
        cell_attrs.ta_bg_color = vc.ansi_to_theme_color(cell_attrs.ta_bg_color);
        auto chan = view_colors::to_channels(cell_attrs);
        ncplane_set_cell_yx(window, y, x + lpc, cell_attrs.ta_attrs, chan);
#if 0
        if (desired_fg == desired_bg) {
            if (desired_bg >= 0
                && desired_bg
                    < view_colors::vc_active_palette->tc_palette.size())
            {
                auto adjusted_color
                    = view_colors::vc_active_palette->tc_palette[desired_bg]
                          .xc_lab_color;
                if (adjusted_color.lc_l < 50.0) {
                    adjusted_color.lc_l += 50.0;
                } else {
                    adjusted_color.lc_l -= 50.0;
                }
                bg_color[lpc] = view_colors::vc_active_palette->match_color(
                    adjusted_color);
            }
        } else if (fg_color[lpc] >= 0
                   && fg_color[lpc]
                       < view_colors::vc_active_palette->tc_palette.size()
                   && bg_color[lpc] == -1
                   && base_attrs.ta_bg_color.value_or(0) >= 0
                   && base_attrs.ta_bg_color.value_or(0)
                       < view_colors::vc_active_palette->tc_palette.size())
        {
            const auto& fg_color_info
                = view_colors::vc_active_palette->tc_palette.at(fg_color[lpc]);
            const auto& bg_color_info
                = view_colors::vc_active_palette->tc_palette.at(
                    base_attrs.ta_bg_color.value_or(0));

            if (!fg_color_info.xc_lab_color.sufficient_contrast(
                    bg_color_info.xc_lab_color))
            {
                auto adjusted_color = bg_color_info.xc_lab_color;
                adjusted_color.lc_l = std::max(0.0, adjusted_color.lc_l - 40.0);
                auto new_bg = view_colors::vc_active_palette->match_color(
                    adjusted_color);
                for (int lpc2 = lpc; lpc2 < line_width_chars; lpc2++) {
                    if (fg_color[lpc2] == fg_color[lpc] && bg_color[lpc2] == -1)
                    {
                        bg_color[lpc2] = new_bg;
                    }
                }
            }
        }

        if (fg_color[lpc] == -1) {
            fg_color[lpc] = cur_fg;
        }
        if (bg_color[lpc] == -1) {
            bg_color[lpc] = cur_bg;
        }
#endif
    }

    return retval;
}

view_colors&
view_colors::singleton()
{
    static view_colors s_vc;

    return s_vc;
}

view_colors::view_colors()
    : vc_ansi_to_theme{
          styling::color_unit::from_palette({0}),
          styling::color_unit::from_palette({1}),
          styling::color_unit::from_palette({2}),
          styling::color_unit::from_palette({3}),
          styling::color_unit::from_palette({4}),
          styling::color_unit::from_palette({5}),
          styling::color_unit::from_palette({6}),
          styling::color_unit::from_palette({7}),
      }
{
    auto text_default = text_attrs{};
    text_default.ta_fg_color = styling::color_unit::from_palette(COLOR_WHITE);
    text_default.ta_bg_color = styling::color_unit::from_palette(COLOR_BLACK);
    this->vc_role_attrs[lnav::enums::to_underlying(role_t::VCR_TEXT)]
        = role_attrs{text_default, text_default};
}

block_elem_t
view_colors::wchar_for_icon(ui_icon_t ic) const
{
    return this->vc_icons[lnav::enums::to_underlying(ic)];
}

bool view_colors::initialized = false;

static const std::string COLOR_NAMES[] = {
    "black",
    "red",
    "green",
    "yellow",
    "blue",
    "magenta",
    "cyan",
    "white",
};

class ui_listener : public lnav_config_listener {
public:
    ui_listener() : lnav_config_listener(__FILE__) {}

    void reload_config(error_reporter& reporter) override
    {
        if (!view_colors::initialized) {
            view_colors::vc_active_palette = ansi_colors();
        }

        auto& vc = view_colors::singleton();

        for (const auto& pair : lnav_config.lc_ui_theme_defs) {
            vc.init_roles(pair.second, reporter);
        }

        const auto iter
            = lnav_config.lc_ui_theme_defs.find(lnav_config.lc_ui_theme);

        if (iter == lnav_config.lc_ui_theme_defs.end()) {
            auto theme_names
                = lnav_config.lc_ui_theme_defs | lnav::itertools::first();

            reporter(&lnav_config.lc_ui_theme,
                     lnav::console::user_message::error(
                         attr_line_t("unknown theme -- ")
                             .append_quoted(lnav_config.lc_ui_theme))
                         .with_help(attr_line_t("The available themes are: ")
                                        .join(theme_names, ", ")));

            vc.init_roles(lnav_config.lc_ui_theme_defs["default"], reporter);
            return;
        }

        if (view_colors::initialized) {
            vc.init_roles(iter->second, reporter);
        }
    }
};

std::optional<lab_color>
view_colors::to_lab_color(const styling::color_unit& color)
{
    if (color.cu_value.is<palette_color>()) {
        auto pal_index = color.cu_value.get<palette_color>();
        if (pal_index < vc_active_palette->tc_palette.size()) {
            if (this->vc_notcurses != nullptr && pal_index == COLOR_BLACK) {
                // We use this as the default background, so try to get the
                // real default from the terminal.
                uint32_t chan = 0;
                notcurses_default_background(this->vc_notcurses, &chan);

                unsigned r = 0, g = 0, b = 0;

                ncchannel_rgb8(chan, &r, &g, &b);
                auto rgb = rgb_color{(short) r, (short) g, (short) b};
                return lab_color{rgb};
            }

            return vc_active_palette->tc_palette[pal_index].xc_lab_color;
        }
    } else if (color.cu_value.is<rgb_color>()) {
        auto rgb = color.cu_value.get<rgb_color>();

        return lab_color{rgb};
    }

    return std::nullopt;
}

uint64_t
view_colors::to_channels(const text_attrs& ta)
{
    uint64_t retval = 0;
    ta.ta_fg_color.cu_value.match(
        [&retval](styling::transparent) {
            ncchannels_set_fg_alpha(&retval, NCALPHA_TRANSPARENT);
        },
        [&retval](styling::semantic) {
            ncchannels_set_fg_alpha(&retval, NCALPHA_TRANSPARENT);
        },
        [&retval](const palette_color& pc) {
            if (pc == COLOR_WHITE) {
                ncchannels_set_fg_default(&retval);
            } else {
                ncchannels_set_fg_palindex(&retval, pc);
            }
        },
        [&retval](const rgb_color& rc) {
            ncchannels_set_fg_rgb8(&retval, rc.rc_r, rc.rc_g, rc.rc_b);
        });
    ta.ta_bg_color.cu_value.match(
        [&retval](styling::transparent) {
            ncchannels_set_bg_alpha(&retval, NCALPHA_TRANSPARENT);
        },
        [&retval](styling::semantic) {
            ncchannels_set_bg_alpha(&retval, NCALPHA_TRANSPARENT);
        },
        [&retval](const palette_color& pc) {
            if (pc == COLOR_BLACK) {
                ncchannels_set_bg_default(&retval);
            } else {
                ncchannels_set_bg_palindex(&retval, pc);
            }
        },
        [&retval](const rgb_color& rc) {
            ncchannels_set_bg_rgb8(&retval, rc.rc_r, rc.rc_g, rc.rc_b);
        });

    return retval;
}

static ui_listener _UI_LISTENER;
term_color_palette* view_colors::vc_active_palette;

void
view_colors::init(notcurses* nc)
{
    vc_active_palette = ansi_colors();
    if (nc != nullptr) {
        const auto* caps = notcurses_capabilities(nc);
        if (caps->rgb) {
            log_info("terminal supports RGB colors");
        } else {
            log_info("terminal supports %d colors", caps->colors);
        }
        if (caps->colors > 8) {
            vc_active_palette = xterm_colors();
        }
    }

    singleton().vc_notcurses = nc;
    initialized = true;

    {
        auto reporter
            = [](const void*, const lnav::console::user_message& um) {};

        _UI_LISTENER.reload_config(reporter);
    }
}

styling::color_unit
view_colors::match_color(styling::color_unit cu) const
{
    if (this->vc_notcurses == nullptr) {
        return cu;
    }

    const auto* caps = notcurses_capabilities(this->vc_notcurses);

    if (caps->rgb) {
        return cu;
    }

    if (cu.cu_value.is<rgb_color>()) {
        auto rgb = cu.cu_value.get<rgb_color>();
        auto lab = lab_color{rgb};
        auto pal = vc_active_palette->match_color(lab);

        log_trace("mapped RGB (%d, %d, %d) to palette %d",
                  rgb.rc_r,
                  rgb.rc_g,
                  rgb.rc_b,
                  pal);
        return styling::color_unit::from_palette(palette_color{pal});
    }

    return cu;
}

view_colors::role_attrs
view_colors::to_attrs(const lnav_theme& lt,
                      const positioned_property<style_config>& pp_sc,
                      lnav_config_listener::error_reporter& reporter)
{
    const auto& sc = pp_sc.pp_value;
    std::string fg_color, bg_color;
    intern_string_t role_class;

    if (pp_sc.pp_path.empty()) {
#if 0
        // too slow to do this now
        reporter(&sc.sc_color, lnav::console::user_message::warning(""));
#endif
    } else if (!pp_sc.pp_path.empty()) {
        auto role_class_path = pp_sc.pp_path.to_string_fragment();
        auto inner
            = role_class_path.rsplit_pair(string_fragment::tag1{'/'}).value();
        auto outer
            = inner.first.rsplit_pair(string_fragment::tag1{'/'}).value();

        role_class = intern_string::lookup(
            fmt::format(FMT_STRING("-lnav_{}_{}"), outer.second, inner.second));
    }

    auto fg1 = sc.sc_color;
    auto bg1 = sc.sc_background_color;
    shlex(fg1).eval(fg_color, scoped_resolver{&lt.lt_vars});
    shlex(bg1).eval(bg_color, scoped_resolver{&lt.lt_vars});

    auto fg = styling::color_unit::from_str(fg_color).unwrapOrElse(
        [&](const auto& msg) {
            reporter(
                &sc.sc_color,
                lnav::console::user_message::error(
                    attr_line_t("invalid color -- ").append_quoted(sc.sc_color))
                    .with_reason(msg));
            return styling::color_unit::EMPTY;
        });
    auto bg = styling::color_unit::from_str(bg_color).unwrapOrElse(
        [&](const auto& msg) {
            reporter(&sc.sc_background_color,
                     lnav::console::user_message::error(
                         attr_line_t("invalid background color -- ")
                             .append_quoted(sc.sc_background_color))
                         .with_reason(msg));
            return styling::color_unit::EMPTY;
        });

    fg = this->match_color(fg);
    bg = this->match_color(bg);

    auto retval1 = text_attrs{0, fg, bg};
    text_attrs retval2;

    if (sc.sc_underline) {
        retval1 |= text_attrs::style::underline;
        retval2 |= text_attrs::style::underline;
    }
    if (sc.sc_bold) {
        retval1 |= text_attrs::style::bold;
        retval2 |= text_attrs::style::bold;
    }
    if (sc.sc_italic) {
        retval1 |= text_attrs::style::italic;
        retval2 |= text_attrs::style::italic;
    }
    if (sc.sc_strike) {
        retval1 |= text_attrs::style::struck;
        retval2 |= text_attrs::style::struck;
    }

    return {retval1, retval2, role_class};
}

void
view_colors::init_roles(const lnav_theme& lt,
                        lnav_config_listener::error_reporter& reporter)
{
    const auto& default_theme = lnav_config.lc_ui_theme_defs["default"];
    std::string err;

    size_t icon_index = 0;
    for (const auto& ic : {
             lt.lt_icon_hidden,
             lt.lt_icon_ok,
             lt.lt_icon_info,
             lt.lt_icon_warning,
             lt.lt_icon_error,

             lt.lt_icon_log_level_trace,
             lt.lt_icon_log_level_debug,
             lt.lt_icon_log_level_info,
             lt.lt_icon_log_level_stats,
             lt.lt_icon_log_level_notice,
             lt.lt_icon_log_level_warning,
             lt.lt_icon_log_level_error,
             lt.lt_icon_log_level_critical,
             lt.lt_icon_log_level_fatal,

             lt.lt_icon_play,
             lt.lt_icon_edit,
         })
    {
        size_t index = 0;
        if (ic.pp_value.ic_value) {
            auto read_res = ww898::utf::utf8::read([&ic, &index]() {
                return ic.pp_value.ic_value.value()[index++];
            });
            if (read_res.isErr()) {
                reporter(&ic,
                         lnav::console::user_message::error(
                             "icon is not valid UTF-8"));
            } else if (read_res.unwrap() != 0) {
                role_t icon_role;
                switch (static_cast<ui_icon_t>(icon_index)) {
                    case ui_icon_t::hidden:
                        icon_role = role_t::VCR_HIDDEN;
                        break;
                    case ui_icon_t::ok:
                        icon_role = role_t::VCR_OK;
                        break;
                    case ui_icon_t::info:
                        icon_role = role_t::VCR_INFO;
                        break;
                    case ui_icon_t::warning:
                    case ui_icon_t::log_level_warning:
                        icon_role = role_t::VCR_WARNING;
                        break;
                    case ui_icon_t::error:
                    case ui_icon_t::log_level_error:
                    case ui_icon_t::log_level_fatal:
                    case ui_icon_t::log_level_critical:
                        icon_role = role_t::VCR_ERROR;
                        break;
                    case ui_icon_t::play:
                        icon_role = role_t::VCR_OK;
                        break;
                    default:
                        icon_role = role_t::VCR_TEXT;
                        break;
                }
                this->vc_icons[icon_index]
                    = block_elem_t{(char32_t) read_res.unwrap(), icon_role};
            }
        }
        icon_index += 1;
    }

    /* Setup the mappings from roles to actual colors. */
    this->get_role_attrs(role_t::VCR_TEXT)
        = this->to_attrs(lt, lt.lt_style_text, reporter);

    for (int ansi_fg = 1; ansi_fg < 8; ansi_fg++) {
        auto fg_iter = lt.lt_vars.find(COLOR_NAMES[ansi_fg]);
        auto fg_str = fg_iter == lt.lt_vars.end()
            ? ""
            : fmt::to_string(fg_iter->second);

        auto rgb_fg = from<rgb_color>(string_fragment::from_str(fg_str))
                          .unwrapOrElse([&](const auto& msg) {
                              reporter(&fg_str,
                                       lnav::console::user_message::error(
                                           attr_line_t("invalid color -- ")
                                               .append_quoted(fg_str))
                                           .with_reason(msg));
                              return rgb_color{};
                          });

        auto fg = vc_active_palette->match_color(lab_color(rgb_fg));

        if (rgb_fg.empty()) {
            fg = ansi_fg;
        }

        this->vc_ansi_to_theme[ansi_fg] = palette_color{fg};
    }

#if 0
    if (lnav_config.lc_ui_dim_text) {
        this->get_role_attrs(role_t::VCR_TEXT).ra_normal.ta_attrs |= A_DIM;
        this->get_role_attrs(role_t::VCR_TEXT).ra_reverse.ta_attrs |= A_DIM;
    }
#endif
    this->get_role_attrs(role_t::VCR_SEARCH)
        = role_attrs{text_attrs::with_reverse(), text_attrs::with_reverse()};
    this->get_role_attrs(role_t::VCR_SEARCH).ra_class_name
        = intern_string::lookup("-lnav_styles_search");
    this->get_role_attrs(role_t::VCR_IDENTIFIER)
        = this->to_attrs(lt, lt.lt_style_identifier, reporter);
    this->get_role_attrs(role_t::VCR_OK)
        = this->to_attrs(lt, lt.lt_style_ok, reporter);
    this->get_role_attrs(role_t::VCR_INFO)
        = this->to_attrs(lt, lt.lt_style_info, reporter);
    this->get_role_attrs(role_t::VCR_ERROR)
        = this->to_attrs(lt, lt.lt_style_error, reporter);
    this->get_role_attrs(role_t::VCR_WARNING)
        = this->to_attrs(lt, lt.lt_style_warning, reporter);
    this->get_role_attrs(role_t::VCR_ALT_ROW)
        = this->to_attrs(lt, lt.lt_style_alt_text, reporter);
    this->get_role_attrs(role_t::VCR_HIDDEN)
        = this->to_attrs(lt, lt.lt_style_hidden, reporter);
    this->get_role_attrs(role_t::VCR_CURSOR_LINE)
        = this->to_attrs(lt, lt.lt_style_cursor_line, reporter);
    if (this->get_role_attrs(role_t::VCR_CURSOR_LINE).ra_normal.empty()) {
        this->get_role_attrs(role_t::VCR_CURSOR_LINE) = this->to_attrs(
            default_theme, default_theme.lt_style_cursor_line, reporter);
    }
    this->get_role_attrs(role_t::VCR_DISABLED_CURSOR_LINE)
        = this->to_attrs(lt, lt.lt_style_disabled_cursor_line, reporter);
    if (this->get_role_attrs(role_t::VCR_DISABLED_CURSOR_LINE)
            .ra_normal.empty())
    {
        this->get_role_attrs(role_t::VCR_DISABLED_CURSOR_LINE)
            = this->to_attrs(default_theme,
                             default_theme.lt_style_disabled_cursor_line,
                             reporter);
    }
    this->get_role_attrs(role_t::VCR_ADJUSTED_TIME)
        = this->to_attrs(lt, lt.lt_style_adjusted_time, reporter);
    this->get_role_attrs(role_t::VCR_SKEWED_TIME)
        = this->to_attrs(lt, lt.lt_style_skewed_time, reporter);
    this->get_role_attrs(role_t::VCR_OFFSET_TIME)
        = this->to_attrs(lt, lt.lt_style_offset_time, reporter);
    this->get_role_attrs(role_t::VCR_TIME_COLUMN)
        = this->to_attrs(lt, lt.lt_style_time_column, reporter);
    this->get_role_attrs(role_t::VCR_POPUP)
        = this->to_attrs(lt, lt.lt_style_popup, reporter);
    this->get_role_attrs(role_t::VCR_POPUP_BORDER)
        = this->to_attrs(lt, lt.lt_style_popup_border, reporter);
    this->get_role_attrs(role_t::VCR_INLINE_CODE)
        = this->to_attrs(lt, lt.lt_style_inline_code, reporter);
    this->get_role_attrs(role_t::VCR_QUOTED_CODE)
        = this->to_attrs(lt, lt.lt_style_quoted_code, reporter);
    this->get_role_attrs(role_t::VCR_CODE_BORDER)
        = this->to_attrs(lt, lt.lt_style_code_border, reporter);

    {
        auto& time_to_text
            = this->get_role_attrs(role_t::VCR_TIME_COLUMN_TO_TEXT);
        auto time_attrs = this->attrs_for_role(role_t::VCR_TIME_COLUMN);
        auto text_attrs = this->attrs_for_role(role_t::VCR_TEXT);
        time_to_text.ra_class_name.clear();

        time_to_text.ra_normal.ta_fg_color = time_attrs.ta_bg_color;
        time_to_text.ra_normal.ta_bg_color = text_attrs.ta_bg_color;

        auto fg_as_lab_opt = to_lab_color(time_attrs.ta_bg_color);
        auto bg_as_lab_opt = to_lab_color(text_attrs.ta_bg_color);
        if (fg_as_lab_opt && bg_as_lab_opt) {
            auto fg_as_lab = fg_as_lab_opt.value();
            auto bg_as_lab = bg_as_lab_opt.value();
            auto diff = fg_as_lab.lc_l - bg_as_lab.lc_l;
            fg_as_lab.lc_l -= diff / 4.0;
            bg_as_lab.lc_l += diff / 4.0;

            time_to_text.ra_normal.ta_fg_color = this->match_color(
                styling::color_unit::from_rgb(fg_as_lab.to_rgb()));
            time_to_text.ra_normal.ta_bg_color = this->match_color(
                styling::color_unit::from_rgb(bg_as_lab.to_rgb()));
        }

        if (bg_as_lab_opt) {
            auto bg_as_lab = bg_as_lab_opt.value();
            std::vector<std::pair<double, size_t>> contrasting;
            for (const auto& [index, tcolor] :
                 lnav::itertools::enumerate(vc_active_palette->tc_palette))
            {
                if (index < 16) {
                    continue;
                }
                if (bg_as_lab.sufficient_contrast(tcolor.xc_lab_color)) {
                    contrasting.emplace_back(
                        bg_as_lab.deltaE(tcolor.xc_lab_color), index);
                }
            }

            log_info("found %zu contrasting colors for highlights",
                     contrasting.size());
            if (contrasting.empty()) {
                for (auto lpc = size_t{0}; lpc < HI_COLOR_COUNT; lpc++) {
                    this->vc_highlight_colors[lpc] = 16;
                }
            } else {
                std::stable_sort(
                    contrasting.begin(), contrasting.end(), std::greater{});
                for (auto lpc = size_t{0}; lpc < HI_COLOR_COUNT; lpc++) {
                    this->vc_highlight_colors[lpc]
                        = contrasting[lpc % contrasting.size()].second;
                }
            }
            if (lt.lt_style_cursor_line.pp_value.sc_background_color.empty()) {
                auto adjusted_cursor = bg_as_lab;
                if (adjusted_cursor.lc_l < 50) {
                    adjusted_cursor.lc_l += 18;
                } else {
                    adjusted_cursor.lc_l -= 15;
                }
                auto new_cursor_bg = this->match_color(
                    styling::color_unit::from_rgb(adjusted_cursor.to_rgb()));
                this->get_role_attrs(role_t::VCR_CURSOR_LINE)
                    .ra_normal.ta_bg_color
                    = new_cursor_bg;
            }
            if (lt.lt_style_popup.pp_value.sc_background_color.empty()) {
                auto adjusted_cursor = bg_as_lab;
                if (adjusted_cursor.lc_l < 50) {
                    adjusted_cursor.lc_l += 30;
                } else {
                    adjusted_cursor.lc_l -= 30;
                }
                auto new_cursor_bg = this->match_color(
                    styling::color_unit::from_rgb(adjusted_cursor.to_rgb()));
                this->get_role_attrs(role_t::VCR_POPUP).ra_normal.ta_bg_color
                    = new_cursor_bg;
            }
            if (lt.lt_style_inline_code.pp_value.sc_background_color.empty()) {
                auto adjusted_cursor = bg_as_lab;
                if (adjusted_cursor.lc_l < 50) {
                    adjusted_cursor.lc_l = 10;
                } else {
                    adjusted_cursor.lc_l -= 25;
                }
                auto new_cursor_bg = this->match_color(
                    styling::color_unit::from_rgb(adjusted_cursor.to_rgb()));
                this->get_role_attrs(role_t::VCR_INLINE_CODE)
                    .ra_normal.ta_bg_color
                    = new_cursor_bg;
            }
            if (lt.lt_style_quoted_code.pp_value.sc_background_color.empty()) {
                auto adjusted_cursor = bg_as_lab;
                if (adjusted_cursor.lc_l < 50) {
                    adjusted_cursor.lc_l = 10;
                } else {
                    adjusted_cursor.lc_l -= 25;
                }
                auto new_cursor_bg = this->match_color(
                    styling::color_unit::from_rgb(adjusted_cursor.to_rgb()));
                this->get_role_attrs(role_t::VCR_QUOTED_CODE)
                    .ra_normal.ta_bg_color
                    = new_cursor_bg;
                this->get_role_attrs(role_t::VCR_CODE_BORDER)
                    .ra_normal.ta_bg_color
                    = new_cursor_bg;
            }
        }
    }
    this->get_role_attrs(role_t::VCR_FILE_OFFSET)
        = this->to_attrs(lt, lt.lt_style_file_offset, reporter);
    this->get_role_attrs(role_t::VCR_INVALID_MSG)
        = this->to_attrs(lt, lt.lt_style_invalid_msg, reporter);

    this->get_role_attrs(role_t::VCR_STATUS)
        = this->to_attrs(lt, lt.lt_style_status, reporter);
    this->get_role_attrs(role_t::VCR_WARN_STATUS)
        = this->to_attrs(lt, lt.lt_style_warn_status, reporter);
    this->get_role_attrs(role_t::VCR_ALERT_STATUS)
        = this->to_attrs(lt, lt.lt_style_alert_status, reporter);
    this->get_role_attrs(role_t::VCR_ACTIVE_STATUS)
        = this->to_attrs(lt, lt.lt_style_active_status, reporter);
    this->get_role_attrs(role_t::VCR_ACTIVE_STATUS2) = role_attrs{
        this->get_role_attrs(role_t::VCR_ACTIVE_STATUS).ra_normal,
        this->get_role_attrs(role_t::VCR_ACTIVE_STATUS).ra_reverse,
    };
    this->get_role_attrs(role_t::VCR_ACTIVE_STATUS2).ra_normal
        |= text_attrs::style::bold;
    this->get_role_attrs(role_t::VCR_ACTIVE_STATUS2).ra_reverse
        |= text_attrs::style::bold;
    this->get_role_attrs(role_t::VCR_STATUS_TITLE)
        = this->to_attrs(lt, lt.lt_style_status_title, reporter);
    this->get_role_attrs(role_t::VCR_STATUS_SUBTITLE)
        = this->to_attrs(lt, lt.lt_style_status_subtitle, reporter);
    this->get_role_attrs(role_t::VCR_STATUS_INFO)
        = this->to_attrs(lt, lt.lt_style_status_info, reporter);

    this->get_role_attrs(role_t::VCR_STATUS_HOTKEY)
        = this->to_attrs(lt, lt.lt_style_status_hotkey, reporter);
    this->get_role_attrs(role_t::VCR_STATUS_TITLE_HOTKEY)
        = this->to_attrs(lt, lt.lt_style_status_title_hotkey, reporter);
    this->get_role_attrs(role_t::VCR_STATUS_DISABLED_TITLE)
        = this->to_attrs(lt, lt.lt_style_status_disabled_title, reporter);

    this->get_role_attrs(role_t::VCR_H1)
        = this->to_attrs(lt, lt.lt_style_header[0], reporter);
    this->get_role_attrs(role_t::VCR_H2)
        = this->to_attrs(lt, lt.lt_style_header[1], reporter);
    this->get_role_attrs(role_t::VCR_H3)
        = this->to_attrs(lt, lt.lt_style_header[2], reporter);
    this->get_role_attrs(role_t::VCR_H4)
        = this->to_attrs(lt, lt.lt_style_header[3], reporter);
    this->get_role_attrs(role_t::VCR_H5)
        = this->to_attrs(lt, lt.lt_style_header[4], reporter);
    this->get_role_attrs(role_t::VCR_H6)
        = this->to_attrs(lt, lt.lt_style_header[5], reporter);
    this->get_role_attrs(role_t::VCR_HR)
        = this->to_attrs(lt, lt.lt_style_hr, reporter);
    this->get_role_attrs(role_t::VCR_HYPERLINK)
        = this->to_attrs(lt, lt.lt_style_hyperlink, reporter);
    this->get_role_attrs(role_t::VCR_LIST_GLYPH)
        = this->to_attrs(lt, lt.lt_style_list_glyph, reporter);
    this->get_role_attrs(role_t::VCR_BREADCRUMB)
        = this->to_attrs(lt, lt.lt_style_breadcrumb, reporter);
    this->get_role_attrs(role_t::VCR_TABLE_BORDER)
        = this->to_attrs(lt, lt.lt_style_table_border, reporter);
    this->get_role_attrs(role_t::VCR_TABLE_HEADER)
        = this->to_attrs(lt, lt.lt_style_table_header, reporter);
    this->get_role_attrs(role_t::VCR_QUOTE_BORDER)
        = this->to_attrs(lt, lt.lt_style_quote_border, reporter);
    this->get_role_attrs(role_t::VCR_QUOTED_TEXT)
        = this->to_attrs(lt, lt.lt_style_quoted_text, reporter);
    this->get_role_attrs(role_t::VCR_FOOTNOTE_BORDER)
        = this->to_attrs(lt, lt.lt_style_footnote_border, reporter);
    this->get_role_attrs(role_t::VCR_FOOTNOTE_TEXT)
        = this->to_attrs(lt, lt.lt_style_footnote_text, reporter);
    this->get_role_attrs(role_t::VCR_SNIPPET_BORDER)
        = this->to_attrs(lt, lt.lt_style_snippet_border, reporter);
    this->get_role_attrs(role_t::VCR_INDENT_GUIDE)
        = this->to_attrs(lt, lt.lt_style_indent_guide, reporter);

    {
        positioned_property<style_config> stitch_sc;

        stitch_sc.pp_value.sc_color
            = lt.lt_style_status_subtitle.pp_value.sc_background_color;
        stitch_sc.pp_value.sc_background_color
            = lt.lt_style_status_title.pp_value.sc_background_color;
        this->get_role_attrs(role_t::VCR_STATUS_STITCH_TITLE_TO_SUB)
            = this->to_attrs(lt, stitch_sc, reporter);
    }
    {
        positioned_property<style_config> stitch_sc;

        stitch_sc.pp_value.sc_color
            = lt.lt_style_status_title.pp_value.sc_background_color;
        stitch_sc.pp_value.sc_background_color
            = lt.lt_style_status_subtitle.pp_value.sc_background_color;
        this->get_role_attrs(role_t::VCR_STATUS_STITCH_SUB_TO_TITLE)
            = this->to_attrs(lt, stitch_sc, reporter);
    }

    {
        positioned_property<style_config> stitch_sc;

        stitch_sc.pp_value.sc_color
            = lt.lt_style_status.pp_value.sc_background_color;
        stitch_sc.pp_value.sc_background_color
            = lt.lt_style_status_subtitle.pp_value.sc_background_color;
        this->get_role_attrs(role_t::VCR_STATUS_STITCH_SUB_TO_NORMAL)
            = this->to_attrs(lt, stitch_sc, reporter);
    }
    {
        positioned_property<style_config> stitch_sc;

        stitch_sc.pp_value.sc_color
            = lt.lt_style_status_subtitle.pp_value.sc_background_color;
        stitch_sc.pp_value.sc_background_color
            = lt.lt_style_status.pp_value.sc_background_color;
        this->get_role_attrs(role_t::VCR_STATUS_STITCH_NORMAL_TO_SUB)
            = this->to_attrs(lt, stitch_sc, reporter);
    }

    {
        positioned_property<style_config> stitch_sc;

        stitch_sc.pp_value.sc_color
            = lt.lt_style_status.pp_value.sc_background_color;
        stitch_sc.pp_value.sc_background_color
            = lt.lt_style_status_title.pp_value.sc_background_color;
        this->get_role_attrs(role_t::VCR_STATUS_STITCH_TITLE_TO_NORMAL)
            = this->to_attrs(lt, stitch_sc, reporter);
    }
    {
        positioned_property<style_config> stitch_sc;

        stitch_sc.pp_value.sc_color
            = lt.lt_style_status_title.pp_value.sc_background_color;
        stitch_sc.pp_value.sc_background_color
            = lt.lt_style_status.pp_value.sc_background_color;
        this->get_role_attrs(role_t::VCR_STATUS_STITCH_NORMAL_TO_TITLE)
            = this->to_attrs(lt, stitch_sc, reporter);
    }

    this->get_role_attrs(role_t::VCR_INACTIVE_STATUS)
        = this->to_attrs(lt, lt.lt_style_inactive_status, reporter);
    this->get_role_attrs(role_t::VCR_INACTIVE_ALERT_STATUS)
        = this->to_attrs(lt, lt.lt_style_inactive_alert_status, reporter);

    this->get_role_attrs(role_t::VCR_FOCUSED)
        = this->to_attrs(lt, lt.lt_style_focused, reporter);
    this->get_role_attrs(role_t::VCR_DISABLED_FOCUSED)
        = this->to_attrs(lt, lt.lt_style_disabled_focused, reporter);
    this->get_role_attrs(role_t::VCR_SCROLLBAR)
        = this->to_attrs(lt, lt.lt_style_scrollbar, reporter);
    {
        positioned_property<style_config> bar_sc;

        bar_sc.pp_value.sc_color = lt.lt_style_error.pp_value.sc_color;
        bar_sc.pp_value.sc_background_color
            = lt.lt_style_scrollbar.pp_value.sc_background_color;
        this->get_role_attrs(role_t::VCR_SCROLLBAR_ERROR)
            = this->to_attrs(lt, bar_sc, reporter);
    }
    {
        positioned_property<style_config> bar_sc;

        bar_sc.pp_value.sc_color = lt.lt_style_warning.pp_value.sc_color;
        bar_sc.pp_value.sc_background_color
            = lt.lt_style_scrollbar.pp_value.sc_background_color;
        this->get_role_attrs(role_t::VCR_SCROLLBAR_WARNING)
            = this->to_attrs(lt, bar_sc, reporter);
    }

    this->get_role_attrs(role_t::VCR_OBJECT_KEY)
        = this->to_attrs(lt, lt.lt_style_object_key, reporter);
    this->get_role_attrs(role_t::VCR_KEYWORD)
        = this->to_attrs(lt, lt.lt_style_keyword, reporter);
    this->get_role_attrs(role_t::VCR_STRING)
        = this->to_attrs(lt, lt.lt_style_string, reporter);
    this->get_role_attrs(role_t::VCR_COMMENT)
        = this->to_attrs(lt, lt.lt_style_comment, reporter);
    this->get_role_attrs(role_t::VCR_DOC_DIRECTIVE)
        = this->to_attrs(lt, lt.lt_style_doc_directive, reporter);
    this->get_role_attrs(role_t::VCR_VARIABLE)
        = this->to_attrs(lt, lt.lt_style_variable, reporter);
    this->get_role_attrs(role_t::VCR_SYMBOL)
        = this->to_attrs(lt, lt.lt_style_symbol, reporter);
    this->get_role_attrs(role_t::VCR_NULL)
        = this->to_attrs(lt, lt.lt_style_null, reporter);
    this->get_role_attrs(role_t::VCR_ASCII_CTRL)
        = this->to_attrs(lt, lt.lt_style_ascii_ctrl, reporter);
    this->get_role_attrs(role_t::VCR_NON_ASCII)
        = this->to_attrs(lt, lt.lt_style_non_ascii, reporter);
    this->get_role_attrs(role_t::VCR_NUMBER)
        = this->to_attrs(lt, lt.lt_style_number, reporter);
    this->get_role_attrs(role_t::VCR_FUNCTION)
        = this->to_attrs(lt, lt.lt_style_function, reporter);
    this->get_role_attrs(role_t::VCR_TYPE)
        = this->to_attrs(lt, lt.lt_style_type, reporter);
    this->get_role_attrs(role_t::VCR_SEP_REF_ACC)
        = this->to_attrs(lt, lt.lt_style_sep_ref_acc, reporter);
    this->get_role_attrs(role_t::VCR_SUGGESTION)
        = this->to_attrs(lt, lt.lt_style_suggestion, reporter);
    this->get_role_attrs(role_t::VCR_SELECTED_TEXT)
        = this->to_attrs(lt, lt.lt_style_selected_text, reporter);
    if (this->get_role_attrs(role_t::VCR_SELECTED_TEXT).ra_normal.empty()) {
        this->get_role_attrs(role_t::VCR_SELECTED_TEXT) = this->to_attrs(
            default_theme, default_theme.lt_style_selected_text, reporter);
    }
    this->get_role_attrs(role_t::VCR_FUZZY_MATCH)
        = this->to_attrs(lt, lt.lt_style_fuzzy_match, reporter);

    this->get_role_attrs(role_t::VCR_RE_SPECIAL)
        = this->to_attrs(lt, lt.lt_style_re_special, reporter);
    this->get_role_attrs(role_t::VCR_RE_REPEAT)
        = this->to_attrs(lt, lt.lt_style_re_repeat, reporter);
    this->get_role_attrs(role_t::VCR_FILE)
        = this->to_attrs(lt, lt.lt_style_file, reporter);

    this->get_role_attrs(role_t::VCR_DIFF_DELETE)
        = this->to_attrs(lt, lt.lt_style_diff_delete, reporter);
    this->get_role_attrs(role_t::VCR_DIFF_ADD)
        = this->to_attrs(lt, lt.lt_style_diff_add, reporter);
    this->get_role_attrs(role_t::VCR_DIFF_SECTION)
        = this->to_attrs(lt, lt.lt_style_diff_section, reporter);

    this->get_role_attrs(role_t::VCR_LOW_THRESHOLD)
        = this->to_attrs(lt, lt.lt_style_low_threshold, reporter);
    this->get_role_attrs(role_t::VCR_MED_THRESHOLD)
        = this->to_attrs(lt, lt.lt_style_med_threshold, reporter);
    this->get_role_attrs(role_t::VCR_HIGH_THRESHOLD)
        = this->to_attrs(lt, lt.lt_style_high_threshold, reporter);

    for (auto level = static_cast<log_level_t>(LEVEL_UNKNOWN + 1);
         level < LEVEL__MAX;
         level = static_cast<log_level_t>(level + 1))
    {
        auto level_iter = lt.lt_level_styles.find(level);

        if (level_iter == lt.lt_level_styles.end()) {
            this->vc_level_attrs[level]
                = role_attrs{text_attrs{}, text_attrs{}};
        } else {
            this->vc_level_attrs[level]
                = this->to_attrs(lt, level_iter->second, reporter);
        }
    }
    this->vc_level_attrs[LEVEL_UNKNOWN] = this->vc_level_attrs[LEVEL_INFO];

    for (int32_t role_index = 0;
         role_index < lnav::enums::to_underlying(role_t::VCR__MAX);
         role_index++)
    {
        const auto& ra = this->vc_role_attrs[role_index];
        if (ra.ra_class_name.empty()) {
            continue;
        }

        this->vc_class_to_role[ra.ra_class_name.to_string()]
            = VC_ROLE.value(role_t(role_index));
    }
    for (int level_index = 0; level_index < LEVEL__MAX; level_index++) {
        const auto& ra = this->vc_level_attrs[level_index];
        if (ra.ra_class_name.empty()) {
            continue;
        }

        this->vc_class_to_role[ra.ra_class_name.to_string()]
            = SA_LEVEL.value(level_index);
    }

    if (this->vc_notcurses) {
        auto& mouse_i = injector::get<xterm_mouse&>();
        mouse_i.set_enabled(
            this->vc_notcurses,
            check_experimental("mouse")
                || lnav_config.lc_mouse_mode == lnav_mouse_mode::enabled);
    }
}

styling::color_unit
view_colors::color_for_ident(const char* str, size_t len) const
{
    auto index = crc32(1, (const Bytef*) str, len);

    if (str[0] == '#' && (len == 4 || len == 7)) {
        auto fg_res
            = styling::color_unit::from_str(string_fragment(str, 0, len));
        if (fg_res.isOk()) {
            return fg_res.unwrap();
        }
    }

    const auto offset = index % HI_COLOR_COUNT;
    if (this->vc_highlight_colors[offset] == 0) {
        return styling::color_unit::EMPTY;
    }
    auto retval = styling::color_unit::from_palette(
        palette_color{static_cast<uint8_t>(this->vc_highlight_colors[offset])});

    return retval;
}

text_attrs
view_colors::attrs_for_ident(const char* str, size_t len) const
{
    auto retval = this->attrs_for_role(role_t::VCR_IDENTIFIER);

    if (retval.ta_fg_color.cu_value.is<styling::semantic>()) {
        retval.ta_fg_color = this->color_for_ident(str, len);
    }
    if (retval.ta_bg_color.cu_value.is<styling::semantic>()) {
        retval.ta_bg_color = this->color_for_ident(str, len);
    }

    return retval;
}

styling::color_unit
view_colors::ansi_to_theme_color(styling::color_unit ansi_fg) const
{
    if (ansi_fg.cu_value.is<palette_color>()) {
        auto pal
            = static_cast<ansi_color>(ansi_fg.cu_value.get<palette_color>());

        if (pal >= ansi_color::black && pal <= ansi_color::white) {
            return this->vc_ansi_to_theme[lnav::enums::to_underlying(pal)];
        }
    }

    return ansi_fg;
}

Result<screen_curses, std::string>
screen_curses::create(const notcurses_options& options)
{
    auto* nc = notcurses_core_init(&options, stdout);
    if (nc == nullptr) {
        return Err(fmt::format(FMT_STRING("unable to initialize notcurses {}"),
                               strerror(errno)));
    }

    auto& mouse_i = injector::get<xterm_mouse&>();
    mouse_i.set_enabled(
        nc,
        check_experimental("mouse")
            || lnav_config.lc_mouse_mode == lnav_mouse_mode::enabled);

    auto_mem<char> term_name;
    term_name = notcurses_detected_terminal(nc);
    log_info("notcurses detected terminal: %s", term_name.in());

    auto retval = screen_curses(nc);

    tcgetattr(STDIN_FILENO, &retval.sc_termios);
    retval.sc_termios.c_cc[VSTART] = 0;
    retval.sc_termios.c_cc[VSTOP] = 0;
#ifdef VDISCARD
    retval.sc_termios.c_cc[VDISCARD] = 0;
#endif
#ifdef VDSUSP
    retval.sc_termios.c_cc[VDSUSP] = 0;
#endif
    tcsetattr(STDIN_FILENO, TCSANOW, &retval.sc_termios);

    return Ok(std::move(retval));
}

screen_curses::screen_curses(screen_curses&& other) noexcept
    : sc_termios(other.sc_termios),
      sc_notcurses(std::exchange(other.sc_notcurses, nullptr))
{
}

extern "C"
{
Terminfo*
terminfo_load_from_internal(const char* term_name)
{
    log_debug("checking for internal terminfo for: %s", term_name);
    for (const auto& tf : lnav_terminfo_files) {
        if (strcmp(tf.get_name(), term_name) != 0) {
            continue;
        }
        log_info("  found internal terminfo!");
        auto sfp = tf.to_string_fragment_producer();
        auto content = sfp->to_string();
        auto* retval = terminfo_parse(content.c_str(), content.size());
        if (retval != nullptr) {
            return retval;
        }
        log_error("  failed to load internal terminfo");
    }

    return nullptr;
}
}
