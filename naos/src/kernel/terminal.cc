#include "kernel/terminal.hpp"
#include "common.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/string.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/clock.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/framebuffer.hpp"
#include "kernel/kernel.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>
#include <limits>

namespace term
{
minimal_terminal *early_terminal = nullptr;
terminal_manager *manager = nullptr;
std::atomic_bool use_stand_terminal = false;

constexpr u8 default_bg_index = 0;
constexpr u8 default_fg_index = 7;

constexpr static u32 color16[] = {0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
                                  0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF};

bool parse_csi(escape_string_state &state)
{
    int val = state.num;
    auto &attr = state.attr;
    if (val == 0)
    {
        attr.additional_flags = 0;
        attr.removal_flags = std::numeric_limits<u32>::max();
        attr.flags = char_attribute::BG_DEFAULT | char_attribute::FG_DEFAULT;
    }
    else if (val == 1)
    {
        attr.additional_flags |= term_flags::BOLD;
    }
    else if (val == 3)
    {
        attr.additional_flags |= term_flags::ITALIC;
    }
    else if (val == 4)
    {
        attr.additional_flags |= term_flags::UNDERLINE;
    }
    else if (val == 5)
    {
        attr.additional_flags |= term_flags::BLINK;
    }
    else if (val == 7)
    {
        attr.additional_flags |= term_flags::REVERSE;
    }
    else if (val >= 30 && val <= 37)
    {
        attr.fg16 = val - 30;
        attr.flags |= char_attribute::FG16;
        attr.flags &= ~char_attribute::FG_DEFAULT;
    }
    else if (val >= 40 && val <= 47)
    {
        attr.bg16 = val - 40;
        attr.flags |= char_attribute::BG16;
        attr.flags &= ~char_attribute::BG_DEFAULT;
    }
    else if (val >= 90 && val <= 97)
    {
        attr.fg16 = val - 90 + 8;
        attr.flags |= char_attribute::FG16;
        attr.flags &= ~char_attribute::FG_DEFAULT;
    }
    else if (val >= 100 && val <= 107)
    {
        attr.bg16 = val - 100 + 8;
        attr.flags |= char_attribute::BG16;
        attr.flags &= ~char_attribute::BG_DEFAULT;
    }
    else if (val == 38)
    {
        ;
    }
    else if (val == 39)
    {
        attr.flags |= char_attribute::FG_DEFAULT;
    }
    else if (val == 48)
    {
    }
    else if (val == 49)
    {
        attr.flags |= char_attribute::BG_DEFAULT;
    }
    else if (val == 21)
    {
        attr.removal_flags |= term_flags::BOLD;
    }
    else if (val == 22)
    {
        attr.removal_flags |= term_flags::BOLD;
    }
    else if (val == 23)
    {
        attr.removal_flags |= term_flags::ITALIC;
    }
    else if (val == 24)
    {
        attr.removal_flags |= term_flags::UNDERLINE;
    }
    else if (val == 25)
    {
        attr.removal_flags |= term_flags::BLINK;
    }
    else if (val == 27)
    {
        attr.removal_flags |= term_flags::REVERSE;
    }
    else if (val == 51)
    {
        attr.additional_flags |= term_flags::FRAME;
    }
    else if (val == 54)
    {
        attr.removal_flags |= term_flags::FRAME;
    }
    else
    {
        state.num = 0;
        return false;
    }
    state.num = 0;
    return true;
}

freelibcxx::optional<freelibcxx::const_string_view> stream_parse_escape_state(escape_string_state &state,
                                                                              freelibcxx::const_string_view str)
{
    freelibcxx::const_string_view sv = str;

    while (sv.size() > 0)
    {
        bool invalid = false;
        char ch = sv[0];
        if (state.state == ascii_state::none)
        {
            if (ch != '\033')
            {
                state.reset();
                return freelibcxx::nullopt;
            }
            state.state = ascii_state::esc;
        }
        else if (state.state == ascii_state::esc)
        {
            if (ch == '[')
            {
                state.state = ascii_state::csi_number;
            }
            else
            {
                state.reset();
                return freelibcxx::nullopt;
            }
        }
        else if (state.state == ascii_state::csi_number)
        {
            unsigned char pch = ch;
            if ((pch >= 0x40 && pch <= 0x7E) || ch == ':' || ch == ';')
            {
                state.state = ascii_state::csi_split;
                continue;
            }
            if (ch >= '0' && ch <= '9')
            {
                state.num = state.num * 10 + ch - '0';
            }
            else
            {
                invalid = true;
            }
        }
        else if (state.state == ascii_state::csi_split)
        {
            if (ch == ';' || ch == ':')
            {
                if (!parse_csi(state))
                {
                    invalid = true;
                }
                else
                {
                    state.state = ascii_state::csi_number;
                }
            }
            else if (ch == 'm')
            {
                if (!parse_csi(state))
                {
                    invalid = true;
                }
                else
                {
                    state.state = ascii_state::end;
                    return sv.substr(1);
                }
            }
            else
            {
                invalid = true;
            }
        }
        else
        {
            break;
        }

        if (invalid)
        {
            state.reset();
            return freelibcxx::nullopt;
        }

        sv = sv.substr(1);
    }

    return freelibcxx::nullopt;
}

void minimal_terminal::make_state(minimal_term_char_t *prev_term_char, minimal_term_char_t *term_char,
                                  char32_t codepoint, char_attribute &attr)
{
    if (codepoint > 255)
    {
        codepoint = 255;
    }
    term_char->ch = codepoint;
    u8 color = 0x00;
    if (prev_term_char != nullptr)
    {
        term_char->flags = (prev_term_char->flags & ~(attr.removal_flags)) | attr.additional_flags;
        color = prev_term_char->color;
    }
    else if ((attr.flags & (~(char_attribute::FG_DEFAULT | char_attribute::BG_DEFAULT))) == 0)
    {
        attr.flags |= char_attribute::FG_DEFAULT | char_attribute::BG_DEFAULT;
    }

    if (attr.flags & char_attribute::FG_DEFAULT)
    {
        attr.fg16 = default_fg_index;
        attr.flags |= char_attribute::FG16;
    }

    if (attr.flags & char_attribute::FG16)
    {
        color = (color & 0xF0) | attr.fg16;
    }

    if (attr.flags & char_attribute::BG_DEFAULT)
    {
        attr.bg16 = default_bg_index;
        attr.flags |= char_attribute::BG16;
    }
    if (attr.flags & char_attribute::BG16)
    {
        color = (color & 0x0F) | attr.bg16 << 4;
    }
    term_char->color = color;
    // ignore 24bit color and bold/italic
}

fb::cell_t minimal_terminal::to_cell(minimal_term_char_t *term_char)
{
    if (term_char == nullptr)
    {
        fb::cell_t cell;
        cell.codepoint = 0;
        cell.flags = 0;
        cell.bg = color16[default_bg_index];
        cell.fg = color16[default_fg_index];
        return cell;
    }

    fb::cell_t cell;
    cell.codepoint = term_char->ch;
    cell.flags = term_char->flags;
    cell.bg = color16[(term_char->color & 0xF0) >> 4];
    cell.fg = color16[(term_char->color & 0x0F)];
    return cell;
}

freelibcxx::tuple<minimal_term_char_t *, int, int> minimal_terminal::previous_term_char(int row, int col)
{
    if (col == 0)
    {
        row--;
        col = max_element_for_line;
    }
    while (row >= 0)
    {
        if (term_char_cols_[row] == 0)
        {
            col = max_element_for_line;
            row--;
            continue;
        }
        if (term_char_cols_[row] < col)
        {
            col = term_char_cols_[row];
        }
        return freelibcxx::make_tuple(get_char(row, col - 1), row, col - 1);
    }
    return freelibcxx::make_tuple(nullptr, row, col);
}

fb::cell_t stand_terminal::to_cell(stand_term_char_t *term_char)
{
    if (term_char == nullptr)
    {
        fb::cell_t cell;
        cell.codepoint = 0;
        cell.flags = 0;
        cell.bg = color16[default_bg_index];
        cell.fg = color16[default_fg_index];
        return cell;
    }
    fb::cell_t cell;
    cell.codepoint = term_char->ch;
    cell.fg = term_char->get_fg();
    cell.bg = term_char->get_bg();
    cell.flags = term_char->get_flags();
    return cell;
}

void stand_terminal::make_state(stand_term_char_t *prev_term_char, stand_term_char_t *term_char, char32_t codepoint,
                                char_attribute &attr)
{
    u32 fg = 0;
    u32 bg = 0;
    term_char->ch = codepoint;

    if (prev_term_char != nullptr)
    {
        term_char->set_flags((prev_term_char->get_flags() & ~(attr.removal_flags)) | attr.additional_flags);
        fg = prev_term_char->get_fg();
        bg = prev_term_char->get_bg();
    }
    else if ((attr.flags & (~(char_attribute::FG_DEFAULT | char_attribute::BG_DEFAULT))) == 0)
    {
        attr.flags |= char_attribute::FG_DEFAULT | char_attribute::BG_DEFAULT;
    }

    if (attr.flags & char_attribute::FG_DEFAULT)
    {
        attr.fg16 = default_fg_index;
        attr.flags |= char_attribute::FG16;
    }

    if (attr.flags & char_attribute::FG16)
    {
        fg = color16[attr.fg16];
    }

    if (attr.flags & char_attribute::BG_DEFAULT)
    {
        attr.bg16 = default_bg_index;
        attr.flags |= char_attribute::BG16;
    }
    if (attr.flags & char_attribute::BG16)
    {
        bg = color16[attr.bg16];
    }

    // todo: color 256

    if (attr.flags & char_attribute::FG)
    {
        fg = attr.fg;
    }
    if (attr.flags & char_attribute::BG)
    {
        bg = attr.bg;
    }

    term_char->set_fg(fg);
    term_char->set_bg(bg);
}

freelibcxx::tuple<stand_term_char_t *, int, int> stand_terminal::previous_term_char(int row, int col)
{
    if (col == 0)
    {
        row--;
        col = viewport().right;
    }
    while (row >= 0)
    {
        if (history_[row].cols.size() == 0)
        {
            row--;
            col = viewport().right;
            continue;
        }
        if ((int)history_[row].cols.size() < col)
        {
            col = history_[row].cols.size();
        }
        return freelibcxx::make_tuple(get_char(row, col - 1), row, col - 1);
    }
    return freelibcxx::make_tuple(nullptr, row, col);
}

void flush_terminal(u64 delta, u64 userdata)
{
    auto &term = manager->active_terminal();
    if (use_stand_terminal)
    {
        term.flush_dirty();
    }
    timer::add_watcher(1000000 / 60, flush_terminal, 0);
}

terminal_manager::terminal_manager(int nums, const fb::framebuffer_backend &backend)
    : terms_(memory::MemoryAllocatorV)
    , backend_(backend)
{
    for (int i = 0; i < nums; i++)
    {
        terms_.push_back(1000);
    }
    switch_term(0);
    timer::add_watcher(1000000 / 60, flush_terminal, 0);
}

stand_terminal &terminal_manager::get(int index) { return terms_[index]; }

void terminal_manager::switch_term(int index)
{
    if (index == cur_)
    {
        return;
    }
    if (cur_ >= 0)
    {
        terms_[cur_].detach_backend();
    }
    if (index > 0)
    {
        if (cmdline::get_bool("debug_stand", false))
        {
            for (;;)
            {
            }
        }
    }
    cur_ = index;
    terms_[cur_].attach_backend(&backend_);
    uctx::UninterruptibleContext icu;
    terms_[cur_].flush_all();
}

terminal_manager *get_terms() { return manager; }

void early_init(kernel_start_args *args)
{
    fb::framebuffer_t fb{
        reinterpret_cast<void *>(args->fb_addr), args->fb_pitch, args->fb_width, args->fb_height, args->fb_bbp,
    };
    early_terminal = arch::device::vga::early_init(fb);
}
void reset_early_paging()
{
    if (cmdline::get_bool("debug_early", false))
    {
        for (;;)
        {
        }
    }
    void *virt = reinterpret_cast<void *>(memory::kernel_vga_bottom_address);
    memory::kernel_vm_info->paging().big_page_map_to(
        virt, 32 * arch::paging::big_pages, memory::va2pa(early_terminal->backend()->fb().ptr),
        arch::paging::flags::writable | arch::paging::flags::cache_disable | arch::paging::flags::write_through, 0);
    early_terminal->backend()->fb().ptr = virt;
}

void reset_panic_term()
{
    early_terminal->reattach_backend();
    use_stand_terminal = false;
}

void init()
{
    manager = memory::MemoryAllocatorV->New<terminal_manager>(7, *early_terminal->backend());
    use_stand_terminal = true;
}

void write_to(freelibcxx::const_string_view sv, int index)
{
    if (use_stand_terminal)
    {
        auto &term = manager->get(index);
        term.push_string(sv);
    }
    else
    {
        early_terminal->push_string(sv);
        early_terminal->flush_dirty();
    }
}

void write_to_klog(freelibcxx::const_string_view sv) { write_to(sv, 0); }

void commit_changes(int index)
{
    if (use_stand_terminal)
    {
        auto &term = manager->get(index);
        term.commit_changes();
    }
}

} // namespace term