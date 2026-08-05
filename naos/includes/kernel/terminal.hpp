#pragma once
#include "freelibcxx/allocator.hpp"
#include "freelibcxx/formatter.hpp"
#include "freelibcxx/iterator.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/unicode.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/clock.hpp"
#include "kernel/framebuffer.hpp"
#include "kernel/kernel.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include <cctype>

namespace term
{
namespace term_flags
{
enum
{
    BOLD = 1,
    ITALIC = 2,
    UNDERLINE = 4,
    BLINK = 8,
    REVERSE = 16,
    FRAME = 32,
    NEW_LINE = 64,
};
}

enum class ascii_state
{
    none,
    esc,
    csi_number,
    csi_split,
    end,
};

struct char_attribute
{
    enum
    {
        FG16 = 1,
        BG16 = 2,
        FG256 = 4,
        BG256 = 8,
        FG = 16,
        BG = 32,
        FG_DEFAULT = 64,
        BG_DEFAULT = 128,
    };
    u8 bg16;
    u8 fg16;
    u8 bg256;
    u8 fg_256;
    u32 bg;
    u32 fg;
    u32 additional_flags = 0;
    u32 removal_flags = 0;
    u32 flags = 0;
};

struct escape_string_state
{
    ascii_state state = ascii_state::none;
    char_attribute attr;
    u32 num = 0;
    u32 rgb = 0;
    char command = 0;
    u32 command_num = 0;
    bool private_mode = false;

    void reset()
    {
        num = 0;
        rgb = 0;
        command = 0;
        command_num = 0;
        private_mode = false;
        state = ascii_state::none;
        attr = char_attribute();
    }
};

struct minimal_term_char_t
{
    // wide char is not supported yet
    char ch;
    // 0b11110000
    // bg_index(4bits) fg_index(4bits)
    u8 color = 0x07;
    u8 flags;
};

freelibcxx::optional<freelibcxx::const_string_view> stream_parse_escape_state(escape_string_state &state,
                                                                              freelibcxx::const_string_view str);

struct rectangle
{
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;

    bool empty() { return right == left || top == bottom; }

    rectangle &operator+=(const rectangle &rect)
    {
        if (!empty())
        {
            left = freelibcxx::min(rect.left, left);
            right = freelibcxx::max(rect.right, right);
            top = freelibcxx::min(rect.top, top);
            bottom = freelibcxx::max(rect.bottom, bottom);
        }
        else
        {
            *this = rect;
        }
        return *this;
    }

    rectangle() = default;
    rectangle(int left, int right, int top, int bottom)
        : left(left)
        , right(right)
        , top(top)
        , bottom(bottom)
    {
    }
};

constexpr u32 placeholder_freq_us = 1300 * 1000;

template <typename CHILD, typename CHAR> class terminal
{
  public:
    terminal() = default;
    terminal(const terminal &) = delete;

    terminal(terminal &&rhs) noexcept
        : row_offset_(rhs.row_offset_)
        , row_(rhs.row_)
        , col_(rhs.col_)
        , rows_(rhs.rows_)
        , cols_(rhs.cols_)
        , lock_row_(rhs.lock_row_)
        , lock_col_(rhs.lock_col_)
        , dirty_(rhs.dirty_)
        , escape_state_(rhs.escape_state_)
        , backend_(rhs.backend_)
        , last_char_(rhs.last_char_)
        , placeholder_time_(rhs.placeholder_time_)
        , enable_placeholder_(rhs.enable_placeholder_)
        , placeholder_show_(rhs.placeholder_show_)
        , placeholder_reset_(rhs.placeholder_reset_)
        , placeholder_row_(rhs.placeholder_row_)
        , placeholder_col_(rhs.placeholder_col_)
        , placeholder_valid_(rhs.placeholder_valid_)
        , drop_(rhs.drop_)
    {
    }
    terminal &operator=(terminal &&rhs) noexcept
    {
        row_offset_ = rhs.row_offset_;
        row_ = rhs.row_;
        col_ = rhs.col_;
        rows_ = rhs.rows_;
        cols_ = rhs.cols_;
        lock_row_ = rhs.lock_row_;
        lock_col_ = rhs.lock_col_;
        dirty_ = rhs.dirty_;
        escape_state_ = rhs.escape_state_;
        backend_ = rhs.backend_;
        last_char_ = rhs.last_char_;
        placeholder_time_ = rhs.placeholder_time_;
        enable_placeholder_ = rhs.enable_placeholder_;
        placeholder_show_ = rhs.placeholder_show_;
        placeholder_reset_ = rhs.placeholder_reset_;
        placeholder_row_ = rhs.placeholder_row_;
        placeholder_col_ = rhs.placeholder_col_;
        placeholder_valid_ = rhs.placeholder_valid_;
        drop_ = rhs.drop_;
        return *this;
    }

    fb::framebuffer_backend *backend() { return backend_; }

    void attach_backend(fb::framebuffer_backend *backend)
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        auto [rows, cols] = backend->rows_cols();
        backend_ = backend;
        rows_ = rows;
        cols_ = cols;
        dirty_ = viewport();
        placeholder_valid_ = false;
    }
    void reattach_backend()
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        if (backend_ == nullptr)
        {
            return;
        }
        auto [rows, cols] = backend_->rows_cols();
        rows_ = rows;
        cols_ = cols;
        dirty_ = viewport();
        placeholder_valid_ = false;
    }

    void detach_backend()
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        backend_ = nullptr;
    }

    void push_string(freelibcxx::const_string_view str, bool no_escape = false);

    bool pop_char()
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        return pop_char_inner();
    }

    void flush_all()
    {
        {
            uctx::RawSpinLockUninterruptibleContext icu(lock_);
            dirty_ = viewport();
            placeholder_valid_ = false;
        }
        flush_dirty();
    }

    void flush_dirty()
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        bool update_placeholder = false;
        if (enable_placeholder_)
        {
            auto current = timer::get_high_resolution_time();
            const bool cursor_moved = !placeholder_valid_ || placeholder_row_ != row_ || placeholder_col_ != col_;
            const bool blink = current - placeholder_time_ > placeholder_freq_us;
            if (cursor_moved || blink || placeholder_reset_)
            {
                placeholder_time_ = current;
                if (placeholder_valid_)
                {
                    dirty_ += rectangle(placeholder_col_, placeholder_col_ + 1, placeholder_row_, placeholder_row_ + 1);
                }
                dirty_ += rectangle(col_, col_ + 1, row_, row_ + 1);
                if (placeholder_reset_)
                {
                    placeholder_show_ = true;
                    placeholder_reset_ = false;
                }
                else if (!cursor_moved)
                {
                    placeholder_show_ = !placeholder_show_;
                }
                placeholder_row_ = row_;
                placeholder_col_ = col_;
                placeholder_valid_ = true;
                update_placeholder = true;
            }
        }

        if (dirty_.empty() || backend_ == nullptr)
        {
            return;
        }

        auto view = viewport();
        auto child = static_cast<CHILD *>(this);
        if (dirty_.top < view.top)
        {
            dirty_.top = view.top;
        }
        if (dirty_.bottom > view.bottom)
        {
            dirty_.bottom = view.bottom;
        }
        if (dirty_.left < view.left)
        {
            dirty_.left = view.left;
        }
        if (dirty_.right > view.right)
        {
            dirty_.right = view.right;
        }

        for (int row = dirty_.top; row < dirty_.bottom; row++)
        {
            for (int col = dirty_.left; col < dirty_.right; col++)
            {
                auto term_char = child->get_char(row, col);
                backend_->commit(row - view.top, col - view.left, child->to_cell(term_char));
            }
        }
        if (enable_placeholder_ && update_placeholder && placeholder_valid_ && row_ >= view.top && row_ < view.bottom &&
            col_ >= view.left && col_ < view.right)
        {
            backend_->commit_placeholder(row_ - view.top, col_ - view.left, placeholder_show_);
        }
        dirty_ = rectangle();
    }

    void scroll(int lines)
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        dirty_.top = 0;
        dirty_.bottom = rows_;
        dirty_.left = 0;
        dirty_.right = cols_;

        // move viewport
        row_offset_ += lines;

        if (lines < 0)
        {
            row_offset_ = freelibcxx::max(row_offset_, 0);
        }
        else
        {
            auto child = static_cast<CHILD *>(this);
            row_offset_ = freelibcxx::min(row_offset_, child->max_rows());
        }
    }

    void enable_placeholder()
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        enable_placeholder_ = true;
    }

    void commit_changes()
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        lock_col_ = col_;
        lock_row_ = row_;
    }

  protected:
    void push_string_nolock(freelibcxx::const_string_view str, bool no_escape = false);
    // return if skip current char
    bool goto_next_row(char ch, freelibcxx::const_string_view &str)
    {
        dirty_ += rectangle(col_, col_ + 1, row_, row_ + 1);
        auto child = static_cast<CHILD *>(this);
        child->update_row_cols(row_, col_);
        push_new_line();
        if (ch == '\n')
        {
            str = str.substr(1);
            return true;
        }
        return false;
    }

    void push_char(char32_t codepoint)
    {
        auto child = static_cast<CHILD *>(this);
        auto term_char = child->alloc_char(row_, col_);

        if (drop_)
        {
            drop_ = false;
            child->make_state(&last_char_, term_char, codepoint, escape_state_.attr);
        }
        else
        {
            auto [p, row, col] = child->previous_term_char(row_, col_);
            child->make_state(p, term_char, codepoint, escape_state_.attr);
        }
    }

    void clear_cells(int first_row, int first_col, int last_row, int last_col)
    {
        auto child = static_cast<CHILD *>(this);
        first_row = freelibcxx::max(first_row, 0);
        last_row = freelibcxx::min(last_row, child->max_rows());
        first_col = freelibcxx::max(first_col, 0);
        last_col = freelibcxx::min(last_col, cols_);
        for (int row = first_row; row < last_row; row++)
        {
            const int col_begin = row == first_row ? first_col : 0;
            for (int col = col_begin; col < last_col; col++)
            {
                auto term_char = child->get_char(row, col);
                if (term_char != nullptr)
                    child->free_char(row, col, term_char);
            }
        }
    }

    void apply_escape_command()
    {
        auto child = static_cast<CHILD *>(this);
        const int count = escape_state_.command_num == 0 ? 1 : static_cast<int>(escape_state_.command_num);
        switch (escape_state_.command)
        {
            case 'A':
                row_ = freelibcxx::max(row_ - count, 0);
                break;
            case 'B':
                row_ = freelibcxx::min(row_ + count, child->max_rows() - 1);
                break;
            case 'C':
                col_ = freelibcxx::min(col_ + count, cols_);
                break;
            case 'D':
                col_ = freelibcxx::max(col_ - count, 0);
                break;
            case 'G':
                col_ = freelibcxx::min(freelibcxx::max(count - 1, 0), cols_);
                break;
            case 'H':
            case 'f':
                row_ = row_offset_;
                col_ = 0;
                break;
            case 'F':
                row_ = freelibcxx::max(row_ - count, 0);
                col_ = 0;
                break;
            case 'J':
                if (escape_state_.command_num == 2)
                    clear_cells(row_offset_, 0, child->max_rows(), cols_);
                else if (escape_state_.command_num == 1)
                    clear_cells(row_offset_, 0, row_, col_ + 1);
                else
                    clear_cells(row_, col_, child->max_rows(), cols_);
                dirty_ = viewport();
                break;
            case 'K':
                if (escape_state_.command_num == 1)
                    clear_cells(row_, 0, row_ + 1, col_ + 1);
                else if (escape_state_.command_num == 2)
                    clear_cells(row_, 0, row_ + 1, cols_);
                else
                    clear_cells(row_, col_, row_ + 1, cols_);
                dirty_ += rectangle(0, cols_, row_, row_ + 1);
                break;
            case 'm':
            case 'h':
            case 'l':
            case 'n':
            case 'r':
            case 's':
            case 'u':
                // SGR is already represented by escape_state_.attr.  The
                // remaining commands are accepted as terminal controls so
                // applications do not render their control bytes.
                break;
            default:
                break;
        }
    }

    void move_cursor_back_inner()
    {
        const int old_row = row_;
        const int old_col = col_;
        if (col_ > 0)
        {
            col_--;
        }
        else if (row_ > row_offset_)
        {
            row_--;
            col_ = freelibcxx::max(cols_ - 1, 0);
        }
        dirty_ += rectangle(old_col, old_col + 1, old_row, old_row + 1);
        dirty_ += rectangle(col_, col_ + 1, row_, row_ + 1);
    }

    bool pop_char_inner(bool allow_committed = false)
    {
        auto child = static_cast<CHILD *>(this);
        auto [p, row, col] = child->previous_term_char(row_, col_);
        if (p != nullptr)
        {
            if (!allow_committed && ((row == lock_row_ && col < lock_col_) || row < lock_row_))
            {
                return false;
            }

            last_char_ = *p;
            child->free_char(row, col, p);
            dirty_ += rectangle(col, col + 1, row, row + 1);
            dirty_ += rectangle(col_, col_ + 1, row_, row_ + 1);
            row_ = row;
            col_ = col;
            drop_ = true;
            return true;
        }
        return false;
    }

    rectangle viewport() { return rectangle(0, cols_, row_offset_, row_offset_ + rows_); }

    bool viewport_is_full() { return row_ + 1 >= row_offset_ + rows_; }

    void push_new_line()
    {
        auto child = static_cast<CHILD *>(this);
        if (viewport_is_full())
        {
            dirty_ = viewport();
            // clear top row
            int n = child->pop_history(1);
            if (n == 0)
            {
                row_offset_++;
            }
            col_ = 0;
            lock_row_--;
        }
        else
        {
            row_++;
            col_ = 0;
        }
    }

  private:
    lock::spinlock_t lock_;

    // top offset for viewport
    int row_offset_ = 0;

    // writeable row index
    int row_ = 0;
    // writeable column index
    int col_ = 0;

    // viewport height
    int rows_ = 0;
    // viewport width
    int cols_ = 0;

    int lock_row_ = 0;
    int lock_col_ = 0;

    rectangle dirty_;

    escape_string_state escape_state_;

    fb::framebuffer_backend *backend_ = nullptr;
    CHAR last_char_ = {};

    u64 placeholder_time_ = 0;
    bool enable_placeholder_ = false;
    bool placeholder_show_ = false;
    bool placeholder_reset_ = false;
    int placeholder_row_ = -1;
    int placeholder_col_ = -1;
    bool placeholder_valid_ = false;
    bool drop_ = false;
};

template <typename CHILD, typename CHAR>
void terminal<CHILD, CHAR>::push_string_nolock(freelibcxx::const_string_view str, bool no_escape)
{
    placeholder_reset_ = true;
    auto child = static_cast<CHILD *>(this);
    while (str.size() > 0)
    {
        bool next_row = false;
        char ch = str[0];
        if ((ch == '\033' || (escape_state_.state != ascii_state::none && escape_state_.state != ascii_state::end)) &&
            !no_escape)
        {
            if (escape_state_.state == ascii_state::end)
            {
                escape_state_.state = ascii_state::none;
            }
            // ascii escape char
            auto ret = stream_parse_escape_state(escape_state_, str);

            if (ret.has_value())
            {
                str = ret.value();
                child->pop_escape_string();
                apply_escape_command();
                continue;
            }

            if (escape_state_.state != ascii_state::none)
            {
                if (!child->push_escape_string(str))
                {
                    auto old_str = child->escape_string();
                    push_string_nolock(old_str, true);
                    child->pop_escape_string();
                }
                else
                {
                    break;
                }
            }
            // treat as normal char if no escape found
        }

        if (ch == '\n')
        {
            next_row = true;
        }
        else if (ch == '\r')
        {
            col_ = 0;
            str = str.substr(1);
            continue;
        }
        else if (ch == '\b')
        {
            // Backspace moves the cursor; it does not erase the cell.  This
            // matters for line editors, which use '\b' to return to the
            // insertion point after redrawing a command.
            move_cursor_back_inner();
            str = str.substr(1);
            continue;
        }
        else if (ch == '\a')
        {
            // BEL is a terminal notification, not printable text.  There is
            // no audio bell backend yet, but its control glyph must not leak
            // into the framebuffer (BusyBox uses it for history-limit input).
            str = str.substr(1);
            continue;
        }

        if (!next_row)
        {
            // parses utf8 chars as unicode
            auto span = str.span();
            auto slice = freelibcxx::advance_utf8(span);
            if (!slice.has_value())
            {
                str = str.substr(1);
                continue;
            }
            str = span;
            auto codepoint = freelibcxx::utf8_to_unicode(slice.value());
            if (!codepoint.has_value())
            {
                push_char(0xFE);
            }
            else
            {
                push_char(codepoint.value());
            }
            dirty_ += rectangle(col_, col_ + 1, row_, row_ + 1);
            col_++;
            child->update_row_cols(row_, col_);
        }

        if (unlikely(col_ == cols_))
        {
            next_row = true;
        }
        if (next_row)
        {
            if (goto_next_row(ch, str))
            {
                continue;
            }
        }

        escape_state_.reset();
    }
}

template <typename CHILD, typename CHAR>
void terminal<CHILD, CHAR>::push_string(freelibcxx::const_string_view str, bool no_escape)
{
    uctx::RawSpinLockUninterruptibleContext icu(lock_);
    push_string_nolock(str, no_escape);
}

// minimal terminal support
class minimal_terminal final : public terminal<minimal_terminal, minimal_term_char_t>
{
  public:
    friend class terminal<minimal_terminal, minimal_term_char_t>;

    minimal_terminal() = default;

  private:
    minimal_term_char_t *alloc_char(int row, int col) { return term_chars_ + row * max_element_for_line + col; }
    minimal_term_char_t *get_char(int row, int col)
    {
        if (row >= max_rows())
        {
            return nullptr;
        }
        if (col >= term_char_cols_[row])
        {
            return nullptr;
        }
        return term_chars_ + row * max_element_for_line + col;
    }
    void free_char(int row, int col, minimal_term_char_t *term_char) { term_char->ch = 0; }

    void update_row_cols(int row, int cols) { term_char_cols_[row] = cols; }
    int pop_history(int rows)
    {
        if (rows < max_rows())
        {
            memmove(term_char_cols_, term_char_cols_ + rows, (max_lines - rows) * sizeof(unsigned short));
            memmove(term_chars_, term_chars_ + rows * max_element_for_line,
                    ((max_lines - rows) * max_element_for_line) * sizeof(minimal_term_char_t));
            return rows;
        }
        return 0;
    }
    int max_rows() { return max_lines; }

    bool push_escape_string(freelibcxx::const_string_view sv)
    {
        int size = freelibcxx::min((int)sv.size(), 64 - stack_pos_ - 1);
        if (size != (int)sv.size())
        {
            return false;
        }
        memcpy(stack_ + stack_pos_, sv.data(), size);
        stack_pos_ += size;
        return true;
    }

    freelibcxx::const_string_view escape_string() { return freelibcxx::const_string_view(stack_, stack_pos_); }

    void pop_escape_string() { stack_pos_ = 0; }

    fb::cell_t to_cell(minimal_term_char_t *term_char);

    void make_state(minimal_term_char_t *prev_term_char, minimal_term_char_t *term_char, char32_t codepoint,
                    char_attribute &attr);
    freelibcxx::tuple<minimal_term_char_t *, int, int> previous_term_char(int row, int col);

  private:
    constexpr static size_t max_element_for_line = 240;
    constexpr static size_t max_lines = 68;

    minimal_term_char_t term_chars_[max_lines * max_element_for_line];
    unsigned short term_char_cols_[max_lines] = {0};
    int stack_pos_ = 0;
    char stack_[64];
};

struct stand_term_char_t
{
    char32_t ch = 0;
    void set_fg(u32 fg) { this->fg = (this->fg & 0xFF00'0000) | fg; }
    void set_bg(u32 bg) { this->bg = (this->bg & 0xFF00'0000) | bg; }

    void set_flags(u32 flags)
    {
        u32 low = flags & 0xFF;
        u32 high = (flags >> 8) & 0xFF;
        fg = (this->fg & 0xFF'FFFF) | (low) << 24;
        bg = (this->bg & 0xFF'FFFF) | (high) << 24;
    }

    u32 get_fg() { return this->fg & 0xFF'FFFF; }
    u32 get_bg() { return this->bg & 0xFF'FFFF; }
    u32 get_flags()
    {
        u8 low = this->fg >> 24;
        u8 high = this->bg >> 24;
        return (u32)low | ((u32)high >> 8);
    }

    void add_flags(u32 flags) { set_flags(get_flags() | flags); }

  private:
    u32 fg = 0;
    u32 bg = 0;
};

class stand_terminal final : public terminal<stand_terminal, stand_term_char_t>
{
    friend class terminal<stand_terminal, stand_term_char_t>;

  public:
    struct line_cell
    {
        freelibcxx::vector<stand_term_char_t> cols;
        line_cell(freelibcxx::Allocator *allocator)
            : cols(allocator)
        {
        }
    };

    stand_terminal(int max_history)
        : max_history_(max_history)
        , history_(memory::MemoryAllocatorV)
        , stack_(memory::MemoryAllocatorV)
    {
        enable_placeholder();
    }

    stand_terminal(stand_terminal &&rhs) noexcept
        : terminal<stand_terminal, stand_term_char_t>::terminal(std::move(rhs))
        , max_history_(rhs.max_history_)
        , stack_pos_(rhs.stack_pos_)
        , history_(std::move(rhs.history_))
        , stack_(std::move(rhs.stack_))
    {
    }

    stand_terminal &operator=(stand_terminal &&rhs) noexcept
    {
        static_cast<terminal<stand_terminal, stand_term_char_t> &>(*this) = std::move(rhs);
        max_history_ = rhs.max_history_;
        stack_pos_ = rhs.stack_pos_;
        history_ = std::move(rhs.history_);
        stack_ = std::move(rhs.stack_);
        rhs.stack_pos_ = 0;
        return *this;
    }

    stand_term_char_t *alloc_char(int row, int col)
    {
        while (row >= (int)history_.size())
        {
            history_.push_back(memory::MemoryAllocatorV);
        }
        if (col >= (int)history_[row].cols.size())
        {
            history_[row].cols.resize(col + 1, {});
        }
        return &history_[row].cols[col];
    }
    stand_term_char_t *get_char(int row, int col)
    {
        if (row >= (int)history_.size())
        {
            return nullptr;
        }
        if (col >= (int)history_[row].cols.size())
        {
            return nullptr;
        }
        return &history_[row].cols[col];
    }

    void free_char(int row, int col, stand_term_char_t *term_char) { term_char->ch = 0; }
    void update_row_cols(int row, int cols) {}

    int pop_history(int rows)
    {
        if (rows < max_rows())
        {
            int n = freelibcxx::min(rows, (int)history_.size());
            history_.remove_n_at(0, n);
            return n;
        }
        return 0;
    }
    int max_rows() const { return max_history_; }

    bool push_escape_string(freelibcxx::const_string_view sv)
    {
        if (stack_.size() > 512)
        {
            return false;
        }
        stack_.append(sv);
        return true;
    }

    freelibcxx::const_string_view escape_string() { return stack_.const_view(); }

    void pop_escape_string() { stack_.resize(0); }

    fb::cell_t to_cell(stand_term_char_t *term_char);

    void make_state(stand_term_char_t *prev_term_char, stand_term_char_t *term_char, char32_t codepoint,
                    char_attribute &attr);
    freelibcxx::tuple<stand_term_char_t *, int, int> previous_term_char(int row, int col);

  private:
    int max_history_;
    int stack_pos_ = 0;
    freelibcxx::vector<line_cell> history_;
    freelibcxx::string stack_;
};

class terminal_manager
{
  public:
    static constexpr int kernel_console_index = 0;
    static constexpr int user_terminal_index = 1;
    static constexpr int default_terminal_count = 2;

    terminal_manager(int nums, const fb::framebuffer_backend &backend);
    bool switch_term(int index);
    void flush_active_terminal();
    void push_string(int index, freelibcxx::const_string_view str);
    void commit_changes(int index);
    int term_index() const;
    static int klog_term_index() { return kernel_console_index; }
    int total() const { return terms_.size(); }
    bool valid_index(int index) const { return index >= 0 && index < total(); }
    const fb::framebuffer_backend &backend() const { return backend_; }

  private:
    freelibcxx::vector<stand_terminal> terms_;
    fb::framebuffer_backend backend_;
    mutable lock::spinlock_t manager_lock_;
    int cur_ = -1;
};

void early_init(kernel_start_args *args);
void reset_early_paging();
void reset_panic_term();
void init();

terminal_manager *get_terms();

void write_to(freelibcxx::const_string_view sv, int index);
void write_to_klog(freelibcxx::const_string_view sv);
void commit_changes(int index);
void flush_active_terminal();

} // namespace term
