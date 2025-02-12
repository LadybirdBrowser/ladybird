/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Editor.h"
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/JsonObject.h>
#include <AK/MemoryStream.h>
#include <AK/RedBlackTree.h>
#include <AK/ScopeGuard.h>
#include <AK/ScopedValueRollback.h>
#include <AK/StringBuilder.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibUnicode/Segmenter.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

namespace Line {

Configuration Configuration::from_config(StringView libname)
{
    Configuration configuration;
    auto config_file = Core::ConfigFile::open_for_lib(libname).release_value_but_fixme_should_propagate_errors();

    // Read behavior options.
    auto refresh = config_file->read_entry("behavior", "refresh", "lazy");
    auto operation = config_file->read_entry("behavior", "operation_mode");
    auto bracketed_paste = config_file->read_bool_entry("behavior", "bracketed_paste", true);
    auto default_text_editor = config_file->read_entry("behavior", "default_text_editor");

    Configuration::Flags flags { Configuration::Flags::None };
    if (bracketed_paste)
        flags = static_cast<Flags>(flags | Configuration::Flags::BracketedPaste);

    configuration.set(flags);

    if (refresh.equals_ignoring_ascii_case("lazy"sv))
        configuration.set(Configuration::Lazy);
    else if (refresh.equals_ignoring_ascii_case("eager"sv))
        configuration.set(Configuration::Eager);

    if (operation.equals_ignoring_ascii_case("full"sv))
        configuration.set(Configuration::OperationMode::Full);
    else if (operation.equals_ignoring_ascii_case("noescapesequences"sv))
        configuration.set(Configuration::OperationMode::NoEscapeSequences);
    else if (operation.equals_ignoring_ascii_case("noninteractive"sv))
        configuration.set(Configuration::OperationMode::NonInteractive);
    else
        configuration.set(Configuration::OperationMode::Unset);

    if (!default_text_editor.is_empty())
        configuration.set(DefaultTextEditor { move(default_text_editor) });
    else
        configuration.set(DefaultTextEditor { "/bin/TextEditor" });

    // Read keybinds.

    for (auto& binding_key : config_file->keys("keybinds")) {
        GenericLexer key_lexer(binding_key);
        auto has_ctrl = false;
        auto alt = false;
        auto escape = false;
        Vector<Key> keys;

        while (!key_lexer.is_eof()) {
            unsigned key;
            if (escape) {
                key = key_lexer.consume_escaped_character();
                escape = false;
            } else {
                if (key_lexer.next_is("alt+")) {
                    alt = key_lexer.consume_specific("alt+"sv);
                    continue;
                }
                if (key_lexer.next_is("^[")) {
                    alt = key_lexer.consume_specific("^["sv);
                    continue;
                }
                if (key_lexer.next_is("^")) {
                    has_ctrl = key_lexer.consume_specific("^"sv);
                    continue;
                }
                if (key_lexer.next_is("ctrl+")) {
                    has_ctrl = key_lexer.consume_specific("ctrl+"sv);
                    continue;
                }
                if (key_lexer.next_is("\\")) {
                    escape = true;
                    continue;
                }
                // FIXME: Support utf?
                key = key_lexer.consume();
            }
            if (has_ctrl)
                key = ctrl(key);

            keys.append(Key { key, alt ? Key::Alt : Key::None });
            alt = false;
            has_ctrl = false;
        }

        GenericLexer value_lexer { config_file->read_entry("keybinds", binding_key) };
        StringBuilder value_builder;
        while (!value_lexer.is_eof())
            value_builder.append(value_lexer.consume_escaped_character());
        auto value = value_builder.string_view();
        if (value.starts_with("internal:"sv)) {
            configuration.set(KeyBinding {
                keys,
                KeyBinding::Kind::InternalFunction,
                value.substring_view(9, value.length() - 9) });
        } else {
            configuration.set(KeyBinding {
                keys,
                KeyBinding::Kind::Insertion,
                value });
        }
    }

    return configuration;
}

void Editor::set_default_keybinds()
{
    register_key_input_callback(ctrl('N'), EDITOR_INTERNAL_FUNCTION(search_forwards));
    register_key_input_callback(ctrl('P'), EDITOR_INTERNAL_FUNCTION(search_backwards));
    register_key_input_callback(ctrl('A'), EDITOR_INTERNAL_FUNCTION(go_home));
    register_key_input_callback(ctrl('B'), EDITOR_INTERNAL_FUNCTION(cursor_left_character));
    register_key_input_callback(ctrl('D'), EDITOR_INTERNAL_FUNCTION(erase_character_forwards));
    register_key_input_callback(ctrl('E'), EDITOR_INTERNAL_FUNCTION(go_end));
    register_key_input_callback(ctrl('F'), EDITOR_INTERNAL_FUNCTION(cursor_right_character));
    // ^H: ctrl('H') == '\b'
    register_key_input_callback(ctrl('H'), EDITOR_INTERNAL_FUNCTION(erase_character_backwards));
    // DEL - Some terminals send this instead of ^H.
    register_key_input_callback((char)127, EDITOR_INTERNAL_FUNCTION(erase_character_backwards));
    register_key_input_callback(ctrl('K'), EDITOR_INTERNAL_FUNCTION(erase_to_end));
    register_key_input_callback(ctrl('L'), EDITOR_INTERNAL_FUNCTION(clear_screen));
    register_key_input_callback(ctrl('R'), EDITOR_INTERNAL_FUNCTION(enter_search));
    register_key_input_callback(ctrl(']'), EDITOR_INTERNAL_FUNCTION(search_character_forwards));
    register_key_input_callback(Key { ctrl(']'), Key::Alt }, EDITOR_INTERNAL_FUNCTION(search_character_backwards));
    register_key_input_callback(ctrl('T'), EDITOR_INTERNAL_FUNCTION(transpose_characters));
    register_key_input_callback('\n', EDITOR_INTERNAL_FUNCTION(finish));

    // ^X^E: Edit in external editor
    register_key_input_callback(Vector<Key> { ctrl('X'), ctrl('E') }, EDITOR_INTERNAL_FUNCTION(edit_in_external_editor));

    // ^[.: alt-.: insert last arg of previous command (similar to `!$`)
    register_key_input_callback(Key { '.', Key::Alt }, EDITOR_INTERNAL_FUNCTION(insert_last_words));
    register_key_input_callback(ctrl('Y'), EDITOR_INTERNAL_FUNCTION(insert_last_erased));
    register_key_input_callback(Key { 'b', Key::Alt }, EDITOR_INTERNAL_FUNCTION(cursor_left_word));
    register_key_input_callback(Key { 'f', Key::Alt }, EDITOR_INTERNAL_FUNCTION(cursor_right_word));
    register_key_input_callback(Key { ctrl('B'), Key::Alt }, EDITOR_INTERNAL_FUNCTION(cursor_left_nonspace_word));
    register_key_input_callback(Key { ctrl('F'), Key::Alt }, EDITOR_INTERNAL_FUNCTION(cursor_right_nonspace_word));
    // ^[^H: alt-backspace: backward delete word
    register_key_input_callback(Key { '\b', Key::Alt }, EDITOR_INTERNAL_FUNCTION(erase_alnum_word_backwards));
    register_key_input_callback(Key { 'd', Key::Alt }, EDITOR_INTERNAL_FUNCTION(erase_alnum_word_forwards));
    register_key_input_callback(Key { '\\', Key::Alt }, EDITOR_INTERNAL_FUNCTION(erase_spaces));
    register_key_input_callback(Key { 'c', Key::Alt }, EDITOR_INTERNAL_FUNCTION(capitalize_word));
    register_key_input_callback(Key { 'l', Key::Alt }, EDITOR_INTERNAL_FUNCTION(lowercase_word));
    register_key_input_callback(Key { 'u', Key::Alt }, EDITOR_INTERNAL_FUNCTION(uppercase_word));
    register_key_input_callback(Key { 't', Key::Alt }, EDITOR_INTERNAL_FUNCTION(transpose_words));

    // Register these last to all the user to override the previous key bindings
    // Normally ^W. `stty werase \^n` can change it to ^N (or something else).
    register_key_input_callback(m_termios.c_cc[VWERASE], EDITOR_INTERNAL_FUNCTION(erase_word_backwards));
    // Normally ^U. `stty kill \^n` can change it to ^N (or something else).
    register_key_input_callback(m_termios.c_cc[VKILL], EDITOR_INTERNAL_FUNCTION(kill_line));
    register_key_input_callback(m_termios.c_cc[VERASE], EDITOR_INTERNAL_FUNCTION(erase_character_backwards));
}

Editor::Editor(Configuration configuration)
    : m_configuration(move(configuration))
{
    m_always_refresh = m_configuration.refresh_behavior == Configuration::RefreshBehavior::Eager;
    m_pending_chars = {};
    get_terminal_size();
    m_suggestion_display = make<XtermSuggestionDisplay>(m_num_lines, m_num_columns);
}

Editor::~Editor()
{
    if (m_initialized)
        restore();
}

void Editor::ensure_free_lines_from_origin(size_t count)
{
    if (count > m_num_lines) {
        // FIXME: Implement paging
    }

    if (m_origin_row + count <= m_num_lines)
        return;

    auto diff = m_origin_row + count - m_num_lines - 1;
    out(stderr, "\x1b[{}S", diff);
    fflush(stderr);
    m_origin_row -= diff;
    m_refresh_needed = false;
    m_chars_touched_in_the_middle = 0;
}

void Editor::get_terminal_size()
{
    struct winsize ws;
    ioctl(STDERR_FILENO, TIOCGWINSZ, &ws);
    if (ws.ws_col == 0 || ws.ws_row == 0) {
        // LLDB uses ttys which "work" and then gives us a zero sized
        // terminal which is far from useful
        if (int fd = open("/dev/tty", O_RDONLY); fd != -1) {
            ioctl(fd, TIOCGWINSZ, &ws);
            close(fd);
        }
    }
    m_num_columns = ws.ws_col;
    m_num_lines = ws.ws_row;
}

void Editor::add_to_history(ByteString const& line)
{
    if (line.is_empty())
        return;
    ByteString histcontrol = getenv("HISTCONTROL");
    auto ignoredups = histcontrol == "ignoredups" || histcontrol == "ignoreboth";
    auto ignorespace = histcontrol == "ignorespace" || histcontrol == "ignoreboth";
    if (ignoredups && !m_history.is_empty() && line == m_history.last().entry)
        return;
    if (ignorespace && line.starts_with(' '))
        return;
    if ((m_history.size() + 1) > m_history_capacity)
        m_history.take_first();
    auto now = UnixDateTime::now();
    m_history.append({ line, now.seconds_since_epoch() });
    m_history_dirty = true;
}

ErrorOr<Vector<Editor::HistoryEntry>> Editor::try_load_history(StringView path)
{
    auto history_file_or_error = Core::File::open(path, Core::File::OpenMode::Read);

    // We ignore "No such file or directory" errors, as that is just equivalent to an empty history.
    if (history_file_or_error.is_error() && history_file_or_error.error().is_errno() && history_file_or_error.error().code() == ENOENT)
        return Vector<Editor::HistoryEntry> {};

    auto history_file = history_file_or_error.release_value();
    auto data = TRY(history_file->read_until_eof());
    auto hist = StringView { data };
    Vector<HistoryEntry> history;
    for (auto& str : hist.split_view("\n\n"sv)) {
        auto it = str.find("::"sv).value_or(0);
        auto time = str.substring_view(0, it).to_number<time_t>().value_or(0);
        auto string = str.substring_view(it == 0 ? it : it + 2);
        history.append({ string, time });
    }
    return history;
}

bool Editor::load_history(ByteString const& path)
{
    auto history_or_error = try_load_history(path);
    if (history_or_error.is_error())
        return false;
    auto maybe_error = m_history.try_extend(history_or_error.release_value());
    auto okay = !maybe_error.is_error();
    return okay;
}

template<typename It0, typename It1, typename OutputT, typename LessThan>
static void merge(It0&& begin0, It0 const& end0, It1&& begin1, It1 const& end1, OutputT& output, LessThan less_than)
{
    for (;;) {
        if (begin0 == end0 && begin1 == end1)
            return;

        if (begin0 == end0) {
            auto&& right = *begin1;
            if (output.last().entry != right.entry)
                output.append(right);
            ++begin1;
            continue;
        }

        auto&& left = *begin0;
        if (left.entry.is_whitespace()) {
            ++begin0;
            continue;
        }
        if (begin1 == end1) {
            if (output.last().entry != left.entry)
                output.append(left);
            ++begin0;
            continue;
        }

        auto&& right = *begin1;
        if (less_than(left, right)) {
            if (output.last().entry != left.entry)
                output.append(left);
            ++begin0;
        } else {
            if (output.last().entry != right.entry)
                output.append(right);
            ++begin1;
            if (right.entry == left.entry)
                ++begin0;
        }
    }
}

bool Editor::save_history(ByteString const& path)
{
    // Note: Use a dummy entry to simplify merging.
    Vector<HistoryEntry> final_history { { "", 0 } };
    {
        auto history_or_error = try_load_history(path);
        if (history_or_error.is_error())
            return false;
        Vector<HistoryEntry> old_history = history_or_error.release_value();
        merge(
            old_history.begin(), old_history.end(),
            m_history.begin(), m_history.end(),
            final_history,
            [](HistoryEntry const& left, HistoryEntry const& right) { return left.timestamp < right.timestamp; });
    }

    auto file_or_error = Core::File::open(path, Core::File::OpenMode::Write, 0600);
    if (file_or_error.is_error())
        return false;
    auto file = file_or_error.release_value();
    // Skip the dummy entry:
    for (auto iter = final_history.begin() + 1; iter != final_history.end(); ++iter) {
        auto const& entry = *iter;
        auto buffer = ByteString::formatted("{}::{}\n\n", entry.timestamp, entry.entry);
        auto maybe_error = file->write_until_depleted(buffer.bytes());
        if (maybe_error.is_error())
            return false;
    }

    m_history_dirty = false;
    return true;
}

void Editor::clear_line()
{
    for (size_t i = 0; i < m_cursor; ++i)
        fputc(0x8, stderr);
    fputs("\033[K", stderr);
    fflush(stderr);
    m_chars_touched_in_the_middle = buffer().size();
    m_buffer.clear();
    m_cursor = 0;
    m_inline_search_cursor = m_cursor;
}

void Editor::insert(Utf32View const& string)
{
    for (size_t i = 0; i < string.length(); ++i)
        insert(string.code_points()[i]);
}

void Editor::insert(ByteString const& string)
{
    for (auto ch : Utf8View { string })
        insert(ch);
}

void Editor::insert(StringView string_view)
{
    auto view = Utf8View { string_view };
    insert(view);
}

void Editor::insert(Utf8View& view)
{
    for (auto ch : view)
        insert(ch);
}

void Editor::insert(u32 const cp)
{
    StringBuilder builder;
    builder.append(Utf32View(&cp, 1));
    auto str = builder.to_byte_string();
    if (m_pending_chars.try_append(str.characters(), str.length()).is_error())
        return;

    readjust_anchored_styles(m_cursor, ModificationKind::Insertion);

    if (m_cursor == m_buffer.size()) {
        m_buffer.append(cp);
        m_cursor = m_buffer.size();
        m_inline_search_cursor = m_cursor;
        return;
    }

    m_buffer.insert(m_cursor, cp);
    ++m_chars_touched_in_the_middle;
    ++m_cursor;
    m_inline_search_cursor = m_cursor;
}

void Editor::register_key_input_callback(KeyBinding const& binding)
{
    if (binding.kind == KeyBinding::Kind::InternalFunction) {
        auto internal_function = find_internal_function(binding.binding);
        if (!internal_function) {
            dbgln("LibLine: Unknown internal function '{}'", binding.binding);
            return;
        }
        return register_key_input_callback(binding.keys, move(internal_function));
    }

    return register_key_input_callback(binding.keys, [binding = ByteString(binding.binding)](auto& editor) {
        editor.insert(binding);
        return false;
    });
}

static size_t code_point_length_in_utf8(u32 code_point)
{
    if (code_point <= 0x7f)
        return 1;
    if (code_point <= 0x07ff)
        return 2;
    if (code_point <= 0xffff)
        return 3;
    if (code_point <= 0x10ffff)
        return 4;
    return 3;
}

// buffer [ 0 1 2 3 . . . A . . . B . . . M . . . N ]
//                        ^       ^       ^       ^
//                        |       |       |       +- end of buffer
//                        |       |       +- scan offset = M
//                        |       +- range end = M - B
//                        +- range start = M - A
// This method converts a byte range defined by [start_byte_offset, end_byte_offset] to a code_point range [M - A, M - B] as shown in the diagram above.
// If `reverse' is true, A and B are before M, if not, A and B are after M.
Editor::CodepointRange Editor::byte_offset_range_to_code_point_offset_range(size_t start_byte_offset, size_t end_byte_offset, size_t scan_code_point_offset, bool reverse) const
{
    size_t byte_offset = 0;
    size_t code_point_offset = scan_code_point_offset + (reverse ? 1 : 0);
    CodepointRange range;

    for (;;) {
        if (!reverse) {
            if (code_point_offset >= m_buffer.size())
                break;
        } else {
            if (code_point_offset == 0)
                break;
        }

        if (byte_offset > end_byte_offset)
            break;

        if (byte_offset < start_byte_offset)
            ++range.start;

        if (byte_offset < end_byte_offset)
            ++range.end;

        byte_offset += code_point_length_in_utf8(m_buffer[reverse ? --code_point_offset : code_point_offset++]);
    }

    return range;
}

void Editor::stylize(Span const& span, Style const& style)
{
    if (!span.is_empty())
        return;
    if (style.is_empty())
        return;

    auto start = span.beginning();
    auto end = span.end();

    if (span.mode() == Span::ByteOriented) {
        auto offsets = byte_offset_range_to_code_point_offset_range(start, end, 0);

        start = offsets.start;
        end = offsets.end;
    }

    if (auto maybe_mask = style.mask(); maybe_mask.has_value()) {
        auto it = m_current_masks.find_smallest_not_below_iterator(span.beginning());
        Optional<Style::Mask> last_encountered_entry;
        if (!it.is_end()) {
            // Delete all overlapping old masks.
            while (true) {
                auto next_it = m_current_masks.find_largest_not_above_iterator(span.end());
                if (next_it.is_end())
                    break;
                if (it->has_value())
                    last_encountered_entry = *it;
                m_current_masks.remove(next_it.key());
            }
        }
        m_current_masks.insert(span.beginning(), move(maybe_mask));
        m_current_masks.insert(span.end(), {});
        if (last_encountered_entry.has_value())
            m_current_masks.insert(span.end() + 1, move(last_encountered_entry));
        style.unset_mask();
    }

    auto& spans_starting = style.is_anchored() ? m_current_spans.m_anchored_spans_starting : m_current_spans.m_spans_starting;
    auto& spans_ending = style.is_anchored() ? m_current_spans.m_anchored_spans_ending : m_current_spans.m_spans_ending;

    auto& starting_map = spans_starting.ensure(start);
    if (!starting_map.contains(end))
        m_refresh_needed = true;
    starting_map.set(end, style);

    auto& ending_map = spans_ending.ensure(end);
    if (!ending_map.contains(start))
        m_refresh_needed = true;
    ending_map.set(start, style);
}

void Editor::transform_suggestion_offsets(size_t& invariant_offset, size_t& static_offset, Span::Mode offset_mode) const
{
    auto internal_static_offset = static_offset;
    auto internal_invariant_offset = invariant_offset;
    if (offset_mode == Span::Mode::ByteOriented) {
        // FIXME: We're assuming that invariant_offset points to the end of the available data
        //        this is not necessarily true, but is true in most cases.
        auto offsets = byte_offset_range_to_code_point_offset_range(internal_static_offset, internal_invariant_offset + internal_static_offset, m_cursor - 1, true);

        internal_static_offset = offsets.start;
        internal_invariant_offset = offsets.end - offsets.start;
    }
    invariant_offset = internal_invariant_offset;
    static_offset = internal_static_offset;
}

void Editor::initialize()
{
    if (m_initialized)
        return;

    struct termios termios;
    tcgetattr(0, &termios);
    m_default_termios = termios; // grab a copy to restore

    get_terminal_size();

    if (m_configuration.operation_mode == Configuration::Unset) {
        auto istty = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
        if (!istty) {
            m_configuration.set(Configuration::NonInteractive);
        } else {
            auto* term = getenv("TERM");
            if ((term != NULL) && StringView { term, strlen(term) }.starts_with("xterm"sv))
                m_configuration.set(Configuration::Full);
            else
                m_configuration.set(Configuration::NoEscapeSequences);
        }
    }

    // Because we use our own line discipline which includes echoing,
    // we disable ICANON and ECHO.
    if (m_configuration.operation_mode == Configuration::Full) {
        termios.c_lflag &= ~(ECHO | ICANON);
        tcsetattr(0, TCSANOW, &termios);
    }

    m_termios = termios;

    set_default_keybinds();
    for (auto& keybind : m_configuration.keybindings)
        register_key_input_callback(keybind);

    if (m_configuration.m_signal_mode == Configuration::WithSignalHandlers) {
        m_signal_handlers.append(Core::EventLoop::register_signal(SIGINT, [this](int) {
            Core::EventLoop::current().deferred_invoke([this] { interrupted().release_value_but_fixme_should_propagate_errors(); });
        }));

        m_signal_handlers.append(Core::EventLoop::register_signal(SIGWINCH, [this](int) {
            Core::EventLoop::current().deferred_invoke([this] { resized().release_value_but_fixme_should_propagate_errors(); });
        }));
    }

    m_initialized = true;
}

void Editor::refetch_default_termios()
{
    struct termios termios;
    tcgetattr(0, &termios);
    m_default_termios = termios;
    if (m_configuration.operation_mode == Configuration::Full)
        termios.c_lflag &= ~(ECHO | ICANON);
    m_termios = termios;
}

ErrorOr<void> Editor::interrupted()
{
    if (m_is_searching)
        return m_search_editor->interrupted();

    if (!m_is_editing)
        return {};

    m_was_interrupted = true;
    handle_interrupt_event();
    if (!m_finish || !m_previous_interrupt_was_handled_as_interrupt)
        return {};

    m_finish = false;
    {
        auto stderr_stream = TRY(Core::File::standard_error());
        TRY(reposition_cursor(*stderr_stream, true));
        if (TRY(m_suggestion_display->cleanup())) {
            TRY(reposition_cursor(*stderr_stream, true));
            TRY(cleanup_suggestions());
        }
        TRY(stderr_stream->write_until_depleted("\r"sv.bytes()));
    }
    m_buffer.clear();
    m_chars_touched_in_the_middle = buffer().size();
    m_is_editing = false;
    restore();
    m_notifier->set_enabled(false);
    m_notifier = nullptr;
    Core::EventLoop::current().quit(Retry);
    return {};
}

ErrorOr<void> Editor::resized()
{
    m_was_resized = true;
    m_previous_num_columns = m_num_columns;
    auto old_origin_row = m_origin_row;
    auto old_origin_column = m_origin_column;

    get_terminal_size();

    if (!m_has_origin_reset_scheduled) {
        // Reset the origin, but make sure it doesn't blow up if we can't read it
        if (set_origin(false)) {
            // The origin we have right now actually points to where the cursor should be (in the middle of the buffer somewhere)
            // Find the "true" origin.
            auto current_buffer_metrics = actual_rendered_string_metrics(buffer_view(), m_current_masks);
            auto lines = m_cached_prompt_metrics.lines_with_addition(current_buffer_metrics, m_num_columns);
            auto offset = m_cached_prompt_metrics.offset_with_addition(current_buffer_metrics, m_num_columns);
            if (lines > m_origin_row)
                m_origin_row = 1;
            else
                m_origin_row -= lines - 1; // the prompt and the origin share a line.

            if (offset > m_origin_column)
                m_origin_column = 1;
            else
                m_origin_column -= offset;

            set_origin(m_origin_row, m_origin_column);

            TRY(handle_resize_event(false));
            if (old_origin_column != m_origin_column || old_origin_row != m_origin_row) {
                m_expected_origin_changed = true;
                deferred_invoke([this] {
                    (void)refresh_display();
                });
            }
        } else {
            deferred_invoke([this] { handle_resize_event(true).release_value_but_fixme_should_propagate_errors(); });
            m_has_origin_reset_scheduled = true;
        }
    }

    return {};
}

ErrorOr<void> Editor::handle_resize_event(bool reset_origin)
{
    m_has_origin_reset_scheduled = false;
    if (reset_origin && !set_origin(false)) {
        m_has_origin_reset_scheduled = true;
        deferred_invoke([this] { handle_resize_event(true).release_value_but_fixme_should_propagate_errors(); });
        return {};
    }

    set_origin(m_origin_row, 1);

    auto stderr_stream = TRY(Core::File::standard_error());

    TRY(reposition_cursor(*stderr_stream, true));
    TRY(m_suggestion_display->redisplay(m_suggestion_manager, m_num_lines, m_num_columns));
    m_origin_row = m_suggestion_display->origin_row();
    TRY(reposition_cursor(*stderr_stream));

    if (m_is_searching)
        TRY(m_search_editor->resized());

    return {};
}

ErrorOr<void> Editor::really_quit_event_loop()
{
    m_finish = false;
    {
        auto stderr_stream = TRY(Core::File::standard_error());
        TRY(reposition_cursor(*stderr_stream, true));
        TRY(stderr_stream->write_until_depleted("\n"sv.bytes()));
    }
    auto string = line();
    m_buffer.clear();
    m_chars_touched_in_the_middle = buffer().size();
    m_is_editing = false;

    if (m_initialized)
        restore();

    m_returned_line = string;
    m_notifier->set_enabled(false);
    m_notifier = nullptr;
    Core::EventLoop::current().quit(Exit);
    return {};
}

auto Editor::get_line(ByteString const& prompt) -> Result<ByteString, Editor::Error>
{
    initialize();
    m_is_editing = true;

    if (m_configuration.operation_mode == Configuration::NoEscapeSequences || m_configuration.operation_mode == Configuration::NonInteractive) {
        // Do not use escape sequences, instead, use LibC's getline.
        size_t size = 0;
        char* line = nullptr;
        // Show the prompt only on interactive mode (NoEscapeSequences in this case).
        if (m_configuration.operation_mode != Configuration::NonInteractive)
            fputs(prompt.characters(), stderr);
        auto line_length = getline(&line, &size, stdin);
        // getline() returns -1 and sets errno=0 on EOF.
        if (line_length == -1) {
            if (line)
                free(line);
            if (errno == 0)
                return Error::Eof;

            return Error::ReadFailure;
        }
        restore();
        if (line) {
            ByteString result { line, (size_t)line_length, Chomp };
            free(line);
            return result;
        }

        return Error::ReadFailure;
    }

    auto old_cols = m_num_columns;
    auto old_lines = m_num_lines;
    get_terminal_size();

    if (m_configuration.enable_bracketed_paste)
        fprintf(stderr, "\x1b[?2004h");

    if (m_num_columns != old_cols || m_num_lines != old_lines)
        m_refresh_needed = true;

    set_prompt(prompt);
    reset();
    strip_styles(true);

    {
        auto stderr_stream = Core::File::standard_error().release_value_but_fixme_should_propagate_errors();
        auto prompt_lines = max(current_prompt_metrics().line_metrics.size(), 1ul) - 1;
        for (size_t i = 0; i < prompt_lines; ++i)
            stderr_stream->write_until_depleted("\n"sv.bytes()).release_value_but_fixme_should_propagate_errors();

        VT::move_relative(-static_cast<int>(prompt_lines), 0, *stderr_stream).release_value_but_fixme_should_propagate_errors();
    }

    set_origin();

    m_history_cursor = m_history.size();

    if (auto refresh_result = refresh_display(); refresh_result.is_error())
        m_input_error = Error::ReadFailure;

    Core::EventLoop loop;

    m_notifier = Core::Notifier::construct(STDIN_FILENO, Core::Notifier::Type::Read);

    if (m_input_error.has_value())
        loop.quit(Exit);

    m_notifier->on_activation = [&] {
        if (try_update_once().is_error())
            loop.quit(Exit);
    };

    if (!m_incomplete_data.is_empty()) {
        deferred_invoke([&] {
            if (try_update_once().is_error())
                loop.quit(Exit);
        });
    }

    if (loop.exec() == Retry)
        return get_line(prompt);

    return m_input_error.has_value() ? Result<ByteString, Editor::Error> { m_input_error.value() } : Result<ByteString, Editor::Error> { m_returned_line };
}

ErrorOr<void> Editor::try_update_once()
{
    if (m_was_interrupted) {
        handle_interrupt_event();
    }

    TRY(handle_read_event());

    if (m_always_refresh)
        m_refresh_needed = true;

    TRY(refresh_display());

    if (m_finish)
        TRY(really_quit_event_loop());

    return {};
}

void Editor::handle_interrupt_event()
{
    m_was_interrupted = false;
    m_previous_interrupt_was_handled_as_interrupt = false;

    m_callback_machine.interrupted(*this);
    if (!m_callback_machine.should_process_last_pressed_key())
        return;

    m_previous_interrupt_was_handled_as_interrupt = true;

    fprintf(stderr, "^C\n");
    fflush(stderr);

    if (on_interrupt_handled)
        on_interrupt_handled();

    m_buffer.clear();
    m_chars_touched_in_the_middle = buffer().size();
    m_cursor = 0;
    set_origin(false);

    finish();
}

ErrorOr<void> Editor::handle_read_event()
{
    if (m_prohibit_input_processing) {
        m_have_unprocessed_read_event = true;
        return {};
    }

    auto prohibit_scope = prohibit_input();

    char keybuf[1024];
    ssize_t nread = 0;

    if (!m_incomplete_data.size())
        nread = read(0, keybuf, sizeof(keybuf));

    if (nread < 0) {
        if (errno == EINTR) {
            if (!m_was_interrupted) {
                if (m_was_resized)
                    return {};

                finish();
                return {};
            }

            handle_interrupt_event();
            return {};
        }

        ScopedValueRollback errno_restorer(errno);
        perror("read failed");

        m_input_error = Error::ReadFailure;
        finish();
        return {};
    }

    m_incomplete_data.append(keybuf, nread);
    auto available_bytes = m_incomplete_data.size();

    if (available_bytes == 0) {
        m_input_error = Error::Empty;
        finish();
        return {};
    }

    auto reverse_tab = false;

    // Discard starting bytes until they make sense as utf-8.
    size_t valid_bytes = 0;
    while (available_bytes > 0) {
        Utf8View { StringView { m_incomplete_data.data(), available_bytes } }.validate(valid_bytes);
        if (valid_bytes != 0)
            break;
        m_incomplete_data.take_first();
        --available_bytes;
    }

    Utf8View input_view { StringView { m_incomplete_data.data(), valid_bytes } };
    size_t consumed_code_points = 0;

    static Vector<u8, 4> csi_parameter_bytes;
    static Vector<u8> csi_intermediate_bytes;
    Vector<unsigned, 4> csi_parameters;
    u8 csi_final;
    enum CSIMod {
        Shift = 1,
        Alt = 2,
        Ctrl = 4,
    };

    for (auto code_point : input_view) {
        if (m_finish)
            break;

        ++consumed_code_points;

        if (code_point == 0)
            continue;

        switch (m_state) {
        case InputState::GotEscape:
            switch (code_point) {
            case '[':
                m_state = InputState::CSIExpectParameter;
                continue;
            default: {
                m_callback_machine.key_pressed(*this, { code_point, Key::Alt });
                m_state = InputState::Free;
                TRY(cleanup_suggestions());
                continue;
            }
            }
        case InputState::CSIExpectParameter:
            if (code_point >= 0x30 && code_point <= 0x3f) { // '0123456789:;<=>?'
                csi_parameter_bytes.append(code_point);
                continue;
            }
            m_state = InputState::CSIExpectIntermediate;
            [[fallthrough]];
        case InputState::CSIExpectIntermediate:
            if (code_point >= 0x20 && code_point <= 0x2f) { // ' !"#$%&\'()*+,-./'
                csi_intermediate_bytes.append(code_point);
                continue;
            }
            m_state = InputState::CSIExpectFinal;
            [[fallthrough]];
        case InputState::CSIExpectFinal: {
            m_state = m_previous_free_state;
            auto is_in_paste = m_state == InputState::Paste;
            for (auto& parameter : ByteString::copy(csi_parameter_bytes).split(';')) {
                if (auto value = parameter.to_number<unsigned>(); value.has_value())
                    csi_parameters.append(value.value());
                else
                    csi_parameters.append(0);
            }
            unsigned param1 = 0, param2 = 0;
            if (csi_parameters.size() >= 1)
                param1 = csi_parameters[0];
            if (csi_parameters.size() >= 2)
                param2 = csi_parameters[1];
            unsigned modifiers = param2 ? param2 - 1 : 0;

            if (is_in_paste && code_point != '~' && param1 != 201) {
                // The only valid escape to process in paste mode is the stop-paste sequence.
                // so treat everything else as part of the pasted data.
                insert('\x1b');
                insert('[');
                insert(StringView { csi_parameter_bytes.data(), csi_parameter_bytes.size() });
                insert(StringView { csi_intermediate_bytes.data(), csi_intermediate_bytes.size() });
                insert(code_point);
                continue;
            }
            if (!(code_point >= 0x40 && code_point <= 0x7f)) {
                dbgln("LibLine: Invalid CSI: {:02x} ({:c})", code_point, code_point);
                continue;
            }
            csi_final = code_point;
            csi_parameters.clear();
            csi_parameter_bytes.clear();
            csi_intermediate_bytes.clear();

            if (csi_final == 'Z') {
                // 'reverse tab'
                reverse_tab = true;
                break;
            }
            TRY(cleanup_suggestions());

            switch (csi_final) {
            case 'A': // ^[[A: arrow up
                search_backwards();
                continue;
            case 'B': // ^[[B: arrow down
                search_forwards();
                continue;
            case 'D': // ^[[D: arrow left
                if (modifiers == CSIMod::Alt || modifiers == CSIMod::Ctrl)
                    cursor_left_word();
                else
                    cursor_left_character();
                continue;
            case 'C': // ^[[C: arrow right
                if (modifiers == CSIMod::Alt || modifiers == CSIMod::Ctrl)
                    cursor_right_word();
                else
                    cursor_right_character();
                continue;
            case 'H': // ^[[H: home
                go_home();
                continue;
            case 'F': // ^[[F: end
                go_end();
                continue;
            case 127:
                if (modifiers == CSIMod::Ctrl)
                    erase_alnum_word_backwards();
                else
                    erase_character_backwards();
                continue;
            case '~':
                if (param1 == 3) { // ^[[3~: delete
                    if (modifiers == CSIMod::Ctrl)
                        erase_alnum_word_forwards();
                    else
                        erase_character_forwards();
                    m_search_offset = 0;
                    continue;
                }
                if (m_configuration.enable_bracketed_paste) {
                    // ^[[200~: start bracketed paste
                    // ^[[201~: end bracketed paste
                    if (!is_in_paste && param1 == 200) {
                        m_state = InputState::Paste;
                        continue;
                    }
                    if (is_in_paste && param1 == 201) {
                        m_state = InputState::Free;
                        if (on_paste) {
                            on_paste(Utf32View { m_paste_buffer.data(), m_paste_buffer.size() }, *this);
                            m_paste_buffer.clear_with_capacity();
                        }
                        if (!m_paste_buffer.is_empty())
                            insert(Utf32View { m_paste_buffer.data(), m_paste_buffer.size() });
                        continue;
                    }
                }
                // ^[[5~: page up
                // ^[[6~: page down
                dbgln("LibLine: Unhandled '~': {}", param1);
                continue;
            default:
                dbgln("LibLine: Unhandled final: {:02x} ({:c})", code_point, code_point);
                continue;
            }
            VERIFY_NOT_REACHED();
        }
        case InputState::Verbatim:
            m_state = InputState::Free;
            // Verbatim mode will bypass all mechanisms and just insert the code point.
            insert(code_point);
            continue;
        case InputState::Paste:
            if (code_point == 27) {
                m_previous_free_state = InputState::Paste;
                m_state = InputState::GotEscape;
                continue;
            }
            if (on_paste)
                m_paste_buffer.append(code_point);
            else
                insert(code_point);
            continue;
        case InputState::Free:
            m_previous_free_state = InputState::Free;
            if (code_point == 27) {
                m_callback_machine.key_pressed(*this, code_point);
                // Note that this should also deal with explicitly registered keys
                // that would otherwise be interpreted as escapes.
                if (m_callback_machine.should_process_last_pressed_key())
                    m_state = InputState::GotEscape;
                continue;
            }
            if (code_point == 22) { // ^v
                m_callback_machine.key_pressed(*this, code_point);
                if (m_callback_machine.should_process_last_pressed_key())
                    m_state = InputState::Verbatim;
                continue;
            }
            break;
        }

        // There are no sequences past this point, so short of 'tab', we will want to cleanup the suggestions.
        ArmedScopeGuard suggestion_cleanup { [this] { cleanup_suggestions().release_value_but_fixme_should_propagate_errors(); } };

        // Normally ^D. `stty eof \^n` can change it to ^N (or something else), but Serenity doesn't have `stty` yet.
        // Process this here since the keybinds might override its behavior.
        // This only applies when the buffer is empty. at any other time, the behavior should be configurable.
        if (code_point == m_termios.c_cc[VEOF] && m_buffer.size() == 0) {
            finish_edit();
            continue;
        }

        m_callback_machine.key_pressed(*this, code_point);
        if (!m_callback_machine.should_process_last_pressed_key())
            continue;

        m_search_offset = 0; // reset search offset on any key

        if (code_point == '\t' || reverse_tab) {
            suggestion_cleanup.disarm();

            if (!on_tab_complete)
                continue;

            // Reverse tab can count as regular tab here.
            m_times_tab_pressed++;

            int token_start = m_cursor;

            // Ask for completions only on the first tab
            // and scan for the largest common prefix to display,
            // further tabs simply show the cached completions.
            if (m_times_tab_pressed == 1) {
                m_suggestion_manager.set_suggestions(on_tab_complete(*this));
                m_suggestion_manager.set_start_index(0);
                m_prompt_lines_at_suggestion_initiation = num_lines();
                if (m_suggestion_manager.count() == 0) {
                    // There are no suggestions, beep.
                    fputc('\a', stderr);
                    fflush(stderr);
                }
            }

            // Adjust already incremented / decremented index when switching tab direction.
            if (reverse_tab && m_tab_direction != TabDirection::Backward) {
                m_suggestion_manager.previous();
                m_suggestion_manager.previous();
                m_tab_direction = TabDirection::Backward;
            }
            if (!reverse_tab && m_tab_direction != TabDirection::Forward) {
                m_suggestion_manager.next();
                m_suggestion_manager.next();
                m_tab_direction = TabDirection::Forward;
            }
            reverse_tab = false;

            SuggestionManager::CompletionMode completion_mode;
            switch (m_times_tab_pressed) {
            case 1:
                completion_mode = SuggestionManager::CompletePrefix;
                break;
            case 2:
                completion_mode = SuggestionManager::ShowSuggestions;
                break;
            default:
                completion_mode = SuggestionManager::CycleSuggestions;
                break;
            }

            insert(Utf32View { m_remembered_suggestion_static_data.data(), m_remembered_suggestion_static_data.size() });
            m_remembered_suggestion_static_data.clear_with_capacity();

            auto completion_result = m_suggestion_manager.attempt_completion(completion_mode, token_start);

            auto new_cursor = m_cursor;

            new_cursor += completion_result.new_cursor_offset;
            for (size_t i = completion_result.offset_region_to_remove.start; i < completion_result.offset_region_to_remove.end; ++i)
                remove_at_index(new_cursor);

            new_cursor -= completion_result.static_offset_from_cursor;
            for (size_t i = 0; i < completion_result.static_offset_from_cursor; ++i) {
                m_remembered_suggestion_static_data.append(m_buffer[new_cursor]);
                remove_at_index(new_cursor);
            }

            m_cursor = new_cursor;
            m_inline_search_cursor = new_cursor;
            m_refresh_needed = true;
            m_chars_touched_in_the_middle++;

            for (auto& view : completion_result.insert)
                insert(view);

            auto stderr_stream = TRY(Core::File::standard_error());
            TRY(reposition_cursor(*stderr_stream));

            if (completion_result.style_to_apply.has_value()) {
                // Apply the style of the last suggestion.
                readjust_anchored_styles(m_suggestion_manager.current_suggestion().start_index, ModificationKind::ForcedOverlapRemoval);
                stylize({ m_suggestion_manager.current_suggestion().start_index, m_cursor, Span::Mode::CodepointOriented }, completion_result.style_to_apply.value());
            }

            switch (completion_result.new_completion_mode) {
            case SuggestionManager::DontComplete:
                m_times_tab_pressed = 0;
                m_remembered_suggestion_static_data.clear_with_capacity();
                break;
            case SuggestionManager::CompletePrefix:
                break;
            default:
                ++m_times_tab_pressed;
                break;
            }

            if (m_times_tab_pressed > 1 && m_suggestion_manager.count() > 0) {
                if (TRY(m_suggestion_display->cleanup()))
                    TRY(reposition_cursor(*stderr_stream));

                m_suggestion_display->set_initial_prompt_lines(m_prompt_lines_at_suggestion_initiation);

                TRY(m_suggestion_display->display(m_suggestion_manager));

                m_origin_row = m_suggestion_display->origin_row();
            }

            if (m_times_tab_pressed > 2) {
                if (m_tab_direction == TabDirection::Forward)
                    m_suggestion_manager.next();
                else
                    m_suggestion_manager.previous();
            }

            if (m_suggestion_manager.count() < 2 && !completion_result.avoid_committing_to_single_suggestion) {
                // We have none, or just one suggestion,
                // we should just commit that and continue
                // after it, as if it were auto-completed.
                TRY(reposition_cursor(*stderr_stream, true));
                TRY(cleanup_suggestions());
                m_remembered_suggestion_static_data.clear_with_capacity();
            }
            continue;
        }

        // If we got here, manually cleanup the suggestions and then insert the new code point.
        m_remembered_suggestion_static_data.clear_with_capacity();
        suggestion_cleanup.disarm();
        TRY(cleanup_suggestions());
        insert(code_point);
    }

    if (consumed_code_points == valid_bytes) {
        m_incomplete_data.clear();
    } else {
        auto bytes_to_drop = input_view.byte_offset_of(consumed_code_points + 1);
        for (size_t i = 0; i < bytes_to_drop; ++i)
            m_incomplete_data.take_first();
    }

    if (!m_incomplete_data.is_empty() && !m_finish)
        deferred_invoke([&] { try_update_once().release_value_but_fixme_should_propagate_errors(); });

    return {};
}

ErrorOr<void> Editor::cleanup_suggestions()
{
    if (m_times_tab_pressed != 0) {
        // Apply the style of the last suggestion.
        readjust_anchored_styles(m_suggestion_manager.current_suggestion().start_index, ModificationKind::ForcedOverlapRemoval);
        stylize({ m_suggestion_manager.current_suggestion().start_index, m_cursor, Span::Mode::CodepointOriented }, m_suggestion_manager.current_suggestion().style);
        // We probably have some suggestions drawn,
        // let's clean them up.
        if (TRY(m_suggestion_display->cleanup())) {
            auto stderr_stream = TRY(Core::File::standard_error());
            TRY(reposition_cursor(*stderr_stream));
            m_refresh_needed = true;
        }
        m_suggestion_manager.reset();
        m_suggestion_display->finish();
    }
    m_times_tab_pressed = 0; // Safe to say if we get here, the user didn't press TAB
    return {};
}

bool Editor::search(StringView phrase, bool allow_empty, bool from_beginning)
{
    int last_matching_offset = -1;
    bool found = false;

    // Do not search for empty strings.
    if (allow_empty || phrase.length() > 0) {
        size_t search_offset = m_search_offset;
        for (size_t i = m_history_cursor; i > 0; --i) {
            auto& entry = m_history[i - 1];
            auto contains = from_beginning ? entry.entry.starts_with(phrase) : entry.entry.contains(phrase);
            if (contains) {
                last_matching_offset = i - 1;
                if (search_offset == 0) {
                    found = true;
                    break;
                }
                --search_offset;
            }
        }

        if (!found) {
            fputc('\a', stderr);
            fflush(stderr);
        }
    }

    if (found) {
        // We plan to clear the buffer, so mark the entire thing touched.
        m_chars_touched_in_the_middle = m_buffer.size();
        m_buffer.clear();
        m_cursor = 0;
        insert(m_history[last_matching_offset].entry);
        // Always needed, as we have cleared the buffer above.
        m_refresh_needed = true;
    }

    return found;
}

void Editor::recalculate_origin()
{
    // Changing the columns can affect our origin if
    // the new size is smaller than our prompt, which would
    // cause said prompt to take up more space, so we should
    // compensate for that.
    if (m_cached_prompt_metrics.max_line_length >= m_num_columns) {
        auto added_lines = (m_cached_prompt_metrics.max_line_length + 1) / m_num_columns - 1;
        m_origin_row += added_lines;
    }

    // We also need to recalculate our cursor position,
    // but that will be calculated and applied at the next
    // refresh cycle.
}

ErrorOr<void> Editor::cleanup()
{
    auto current_buffer_metrics = actual_rendered_string_metrics(buffer_view(), m_current_masks);
    auto new_lines = current_prompt_metrics().lines_with_addition(current_buffer_metrics, m_num_columns);
    if (new_lines < m_shown_lines)
        m_extra_forward_lines = max(m_shown_lines - new_lines, m_extra_forward_lines);

    auto stderr_stream = TRY(Core::File::standard_error());
    TRY(reposition_cursor(*stderr_stream, true));
    auto current_line = num_lines() - 1;
    TRY(VT::clear_lines(current_line, m_extra_forward_lines, *stderr_stream));
    m_extra_forward_lines = 0;
    TRY(reposition_cursor(*stderr_stream));
    return {};
}

ErrorOr<void> Editor::refresh_display()
{
    AllocatingMemoryStream output_stream;
    ScopeGuard flush_stream {
        [&] {
            m_shown_lines = current_prompt_metrics().lines_with_addition(m_cached_buffer_metrics, m_num_columns);

            if (output_stream.used_buffer_size() == 0)
                return;

            auto buffer = ByteBuffer::create_uninitialized(output_stream.used_buffer_size()).release_value_but_fixme_should_propagate_errors();
            output_stream.read_until_filled(buffer).release_value_but_fixme_should_propagate_errors();
            fwrite(buffer.data(), sizeof(char), buffer.size(), stderr);
        }
    };

    auto has_cleaned_up = false;
    // Someone changed the window size, figure it out
    // and react to it, we might need to redraw.
    if (m_was_resized) {
        if (m_expected_origin_changed || m_previous_num_columns != m_num_columns) {
            // We need to cleanup and redo everything.
            m_expected_origin_changed = false;
            m_cached_prompt_valid = false;
            m_refresh_needed = true;
            swap(m_previous_num_columns, m_num_columns);
            recalculate_origin();
            TRY(cleanup());
            swap(m_previous_num_columns, m_num_columns);
            has_cleaned_up = true;
        }
        m_was_resized = false;
    }
    // We might be at the last line, and have more than one line;
    // Refreshing the display will cause the terminal to scroll,
    // so note that fact and bring origin up, making sure to
    // reserve the space for however many lines we move it up.
    auto current_num_lines = num_lines();
    if (m_origin_row + current_num_lines > m_num_lines) {
        if (current_num_lines > m_num_lines) {
            for (size_t i = 0; i < m_num_lines; ++i)
                TRY(output_stream.write_until_depleted("\n"sv.bytes()));
            m_origin_row = 0;
        } else {
            auto old_origin_row = m_origin_row;
            m_origin_row = m_num_lines - current_num_lines + 1;
            for (size_t i = 0; i < old_origin_row - m_origin_row; ++i)
                TRY(output_stream.write_until_depleted("\n"sv.bytes()));
        }
    }
    // Do not call hook on pure cursor movement.
    if (m_cached_prompt_valid && !m_refresh_needed && m_pending_chars.size() == 0) {
        // Probably just moving around.
        TRY(reposition_cursor(output_stream));
        m_cached_buffer_metrics = actual_rendered_string_metrics(buffer_view(), m_current_masks);
        m_drawn_end_of_line_offset = m_buffer.size();
        return {};
    }

    if (on_display_refresh)
        on_display_refresh(*this);

    if (m_cached_prompt_valid) {
        if (!m_refresh_needed && m_cursor == m_buffer.size()) {
            // Just write the characters out and continue,
            // no need to refresh the entire line.
            TRY(output_stream.write_until_depleted(m_pending_chars));
            m_pending_chars.clear();
            m_drawn_cursor = m_cursor;
            m_drawn_end_of_line_offset = m_buffer.size();
            m_cached_buffer_metrics = actual_rendered_string_metrics(buffer_view(), m_current_masks);
            m_drawn_spans = m_current_spans;
            return {};
        }
    }

    auto apply_styles = [&, empty_styles = HashMap<u32, Style> {}](size_t i) -> ErrorOr<void> {
        auto& ends = m_current_spans.m_spans_ending.get(i).value_or<>(empty_styles);
        auto& starts = m_current_spans.m_spans_starting.get(i).value_or<>(empty_styles);

        auto& anchored_ends = m_current_spans.m_anchored_spans_ending.get(i).value_or<>(empty_styles);
        auto& anchored_starts = m_current_spans.m_anchored_spans_starting.get(i).value_or<>(empty_styles);

        if (ends.size() || anchored_ends.size()) {
            Style style;

            for (auto& applicable_style : ends)
                style.unify_with(applicable_style.value);

            for (auto& applicable_style : anchored_ends)
                style.unify_with(applicable_style.value);

            // Disable any style that should be turned off.
            TRY(VT::apply_style(style, output_stream, false));

            // Reapply styles for overlapping spans that include this one.
            style = find_applicable_style(i);
            TRY(VT::apply_style(style, output_stream, true));
        }
        if (starts.size() || anchored_starts.size()) {
            Style style;

            for (auto& applicable_style : starts)
                style.unify_with(applicable_style.value);

            for (auto& applicable_style : anchored_starts)
                style.unify_with(applicable_style.value);

            // Set new styles.
            TRY(VT::apply_style(style, output_stream, true));
        }

        return {};
    };

    auto print_character_at = [&](size_t i) {
        Variant<u32, Utf8View> c { Utf8View {} };
        if (auto it = m_current_masks.find_largest_not_above_iterator(i); !it.is_end() && it->has_value()) {
            auto offset = i - it.key();
            if (it->value().mode == Style::Mask::Mode::ReplaceEntireSelection) {
                auto& mask = it->value().replacement_view;
                auto replacement = mask.begin().peek(offset);
                if (!replacement.has_value())
                    return;
                c = replacement.value();
                ++it;
                u32 next_offset = it.is_end() ? m_drawn_end_of_line_offset : it.key();
                if (i + 1 == next_offset)
                    c = mask.unicode_substring_view(offset, mask.length() - offset);
            } else {
                c = it->value().replacement_view;
            }
        } else {
            c = m_buffer[i];
        }
        auto print_single_character = [&](auto c) -> ErrorOr<void> {
            StringBuilder builder;
            bool should_print_masked = is_ascii_control(c) && c != '\n';
            bool should_print_caret = c < 64 && should_print_masked;
            if (should_print_caret)
                builder.appendff("^{:c}", c + 64);
            else if (should_print_masked)
                builder.appendff("\\x{:0>2x}", c);
            else
                builder.append(Utf32View { &c, 1 });

            if (should_print_masked)
                TRY(output_stream.write_until_depleted("\033[7m"sv.bytes()));

            TRY(output_stream.write_until_depleted(builder.string_view().bytes()));

            if (should_print_masked)
                TRY(output_stream.write_until_depleted("\033[27m"sv.bytes()));

            return {};
        };
        c.visit(
            [&](u32 c) { print_single_character(c).release_value_but_fixme_should_propagate_errors(); },
            [&](auto& view) { for (auto c : view) print_single_character(c).release_value_but_fixme_should_propagate_errors(); });
    };

    // If there have been no changes to previous sections of the line (style or text)
    // just append the new text with the appropriate styles.
    if (!m_always_refresh && m_cached_prompt_valid && m_chars_touched_in_the_middle == 0 && m_drawn_spans.contains_up_to_offset(m_current_spans, m_drawn_cursor)) {
        auto initial_style = find_applicable_style(m_drawn_end_of_line_offset);
        TRY(VT::apply_style(initial_style, output_stream));

        for (size_t i = m_drawn_end_of_line_offset; i < m_buffer.size(); ++i) {
            TRY(apply_styles(i));
            print_character_at(i);
        }

        TRY(VT::apply_style(Style::reset_style(), output_stream));
        m_pending_chars.clear();
        m_refresh_needed = false;
        m_cached_buffer_metrics = actual_rendered_string_metrics(buffer_view(), m_current_masks);
        m_chars_touched_in_the_middle = 0;
        m_drawn_cursor = m_cursor;
        m_drawn_end_of_line_offset = m_buffer.size();

        // No need to reposition the cursor, the cursor is already where it needs to be.
        return {};
    }

    if constexpr (LINE_EDITOR_DEBUG) {
        if (m_cached_prompt_valid && m_chars_touched_in_the_middle == 0) {
            auto x = m_drawn_spans.contains_up_to_offset(m_current_spans, m_drawn_cursor);
            dbgln("Contains: {} At offset: {}", x, m_drawn_cursor);
            dbgln("Drawn Spans:");
            for (auto& sentry : m_drawn_spans.m_spans_starting) {
                for (auto& entry : sentry.value) {
                    dbgln("{}-{}: {}", sentry.key, entry.key, entry.value.to_byte_string());
                }
            }
            dbgln("==========================================================================");
            dbgln("Current Spans:");
            for (auto& sentry : m_current_spans.m_spans_starting) {
                for (auto& entry : sentry.value) {
                    dbgln("{}-{}: {}", sentry.key, entry.key, entry.value.to_byte_string());
                }
            }
        }
    }

    // Ouch, reflow entire line.
    if (!has_cleaned_up) {
        TRY(cleanup());
    }
    TRY(VT::move_absolute(m_origin_row, m_origin_column, output_stream));

    TRY(output_stream.write_until_depleted(m_new_prompt.bytes()));

    TRY(VT::clear_to_end_of_line(output_stream));
    StringBuilder builder;
    for (size_t i = 0; i < m_buffer.size(); ++i) {
        TRY(apply_styles(i));
        print_character_at(i);
    }

    TRY(VT::apply_style(Style::reset_style(), output_stream)); // don't bleed to EOL

    m_pending_chars.clear();
    m_refresh_needed = false;
    m_cached_buffer_metrics = actual_rendered_string_metrics(buffer_view(), m_current_masks);
    m_chars_touched_in_the_middle = 0;
    m_drawn_spans = m_current_spans;
    m_drawn_end_of_line_offset = m_buffer.size();
    m_cached_prompt_valid = true;

    TRY(reposition_cursor(output_stream));
    return {};
}

void Editor::strip_styles(bool strip_anchored)
{
    m_current_spans.m_spans_starting.clear();
    m_current_spans.m_spans_ending.clear();
    m_current_masks.clear();
    m_cached_buffer_metrics = actual_rendered_string_metrics(buffer_view(), {});

    if (strip_anchored) {
        m_current_spans.m_anchored_spans_starting.clear();
        m_current_spans.m_anchored_spans_ending.clear();
    }

    m_refresh_needed = true;
}

ErrorOr<void> Editor::reposition_cursor(Stream& stream, bool to_end)
{
    auto cursor = m_cursor;
    auto saved_cursor = m_cursor;
    if (to_end)
        cursor = m_buffer.size();

    m_cursor = cursor;
    m_drawn_cursor = cursor;

    auto line = cursor_line() - 1;
    auto column = offset_in_line();

    ensure_free_lines_from_origin(line);

    VERIFY(column + m_origin_column <= m_num_columns);
    TRY(VT::move_absolute(line + m_origin_row, column + m_origin_column, stream));

    m_cursor = saved_cursor;
    return {};
}

ErrorOr<void> VT::move_absolute(u32 row, u32 col, Stream& stream)
{
    return stream.write_until_depleted(ByteString::formatted("\033[{};{}H", row, col));
}

ErrorOr<void> VT::move_relative(int row, int col, Stream& stream)
{
    char x_op = 'A', y_op = 'D';

    if (row > 0)
        x_op = 'B';
    else
        row = -row;
    if (col > 0)
        y_op = 'C';
    else
        col = -col;

    if (row > 0)
        TRY(stream.write_until_depleted(ByteString::formatted("\033[{}{}", row, x_op)));
    if (col > 0)
        TRY(stream.write_until_depleted(ByteString::formatted("\033[{}{}", col, y_op)));

    return {};
}

Style Editor::find_applicable_style(size_t offset) const
{
    // Walk through our styles and merge all that fit in the offset.
    auto style = Style::reset_style();
    auto unify = [&](auto& entry) {
        if (entry.key >= offset)
            return;
        for (auto& style_value : entry.value) {
            if (style_value.key <= offset)
                return;
            style.unify_with(style_value.value, true);
        }
    };

    for (auto& entry : m_current_spans.m_spans_starting) {
        unify(entry);
    }

    for (auto& entry : m_current_spans.m_anchored_spans_starting) {
        unify(entry);
    }

    return style;
}

ByteString Style::Background::to_vt_escape() const
{
    if (is_default())
        return "";

    if (m_is_rgb) {
        return ByteString::formatted("\e[48;2;{};{};{}m", m_rgb_color[0], m_rgb_color[1], m_rgb_color[2]);
    } else {
        return ByteString::formatted("\e[{}m", (u8)m_xterm_color + 40);
    }
}

ByteString Style::Foreground::to_vt_escape() const
{
    if (is_default())
        return "";

    if (m_is_rgb) {
        return ByteString::formatted("\e[38;2;{};{};{}m", m_rgb_color[0], m_rgb_color[1], m_rgb_color[2]);
    } else {
        return ByteString::formatted("\e[{}m", (u8)m_xterm_color + 30);
    }
}

ByteString Style::Hyperlink::to_vt_escape(bool starting) const
{
    if (is_empty())
        return "";

    return ByteString::formatted("\e]8;;{}\e\\", starting ? m_link : ByteString::empty());
}

void Style::unify_with(Style const& other, bool prefer_other)
{
    // Unify colors.
    if (prefer_other || m_background.is_default())
        m_background = other.background();

    if (prefer_other || m_foreground.is_default())
        m_foreground = other.foreground();

    // Unify graphic renditions.
    if (other.bold())
        set(Bold);

    if (other.italic())
        set(Italic);

    if (other.underline())
        set(Underline);

    // Unify links.
    if (prefer_other || m_hyperlink.is_empty())
        m_hyperlink = other.hyperlink();

    m_is_empty &= other.m_is_empty;
}

ByteString Style::to_byte_string() const
{
    StringBuilder builder;
    builder.append("Style { "sv);

    if (!m_foreground.is_default()) {
        builder.append("Foreground("sv);
        if (m_foreground.m_is_rgb) {
            builder.join(", "sv, m_foreground.m_rgb_color);
        } else {
            builder.appendff("(XtermColor) {}", (int)m_foreground.m_xterm_color);
        }
        builder.append("), "sv);
    }

    if (!m_background.is_default()) {
        builder.append("Background("sv);
        if (m_background.m_is_rgb) {
            builder.join(' ', m_background.m_rgb_color);
        } else {
            builder.appendff("(XtermColor) {}", (int)m_background.m_xterm_color);
        }
        builder.append("), "sv);
    }

    if (bold())
        builder.append("Bold, "sv);

    if (underline())
        builder.append("Underline, "sv);

    if (italic())
        builder.append("Italic, "sv);

    if (!m_hyperlink.is_empty())
        builder.appendff("Hyperlink(\"{}\"), ", m_hyperlink.m_link);

    if (!m_mask.has_value()) {
        builder.appendff("Mask(\"{}\", {}), ",
            m_mask->replacement,
            m_mask->mode == Mask::Mode::ReplaceEntireSelection
                ? "ReplaceEntireSelection"
                : "ReplaceEachCodePointInSelection");
    }

    builder.append('}');

    return builder.to_byte_string();
}

ErrorOr<void> VT::apply_style(Style const& style, Stream& stream, bool is_starting)
{
    if (is_starting) {
        TRY(stream.write_until_depleted(ByteString::formatted("\033[{};{};{}m{}{}{}",
            style.bold() ? 1 : 22,
            style.underline() ? 4 : 24,
            style.italic() ? 3 : 23,
            style.background().to_vt_escape(),
            style.foreground().to_vt_escape(),
            style.hyperlink().to_vt_escape(true))));
    } else {
        TRY(stream.write_until_depleted(style.hyperlink().to_vt_escape(false)));
    }

    return {};
}

ErrorOr<void> VT::clear_lines(size_t count_above, size_t count_below, Stream& stream)
{
    if (count_below + count_above == 0) {
        TRY(stream.write_until_depleted("\033[2K"sv));
    } else {
        // Go down count_below lines.
        if (count_below > 0)
            TRY(stream.write_until_depleted(ByteString::formatted("\033[{}B", count_below)));
        // Then clear lines going upwards.
        for (size_t i = count_below + count_above; i > 0; --i) {
            TRY(stream.write_until_depleted("\033[2K"sv));
            if (i != 1)
                TRY(stream.write_until_depleted("\033[A"sv));
        }
    }

    return {};
}

ErrorOr<void> VT::save_cursor(Stream& stream)
{
    return stream.write_until_depleted("\033[s"sv);
}

ErrorOr<void> VT::restore_cursor(Stream& stream)
{
    return stream.write_until_depleted("\033[u"sv);
}

ErrorOr<void> VT::clear_to_end_of_line(Stream& stream)
{
    return stream.write_until_depleted("\033[K"sv);
}

enum VTState {
    Free = 1,
    Escape = 3,
    Bracket = 5,
    BracketArgsSemi = 7,
    Title = 9,
    URL = 11,
};
static VTState actual_rendered_string_length_step(StringMetrics& metrics, size_t index, StringMetrics::LineMetrics& current_line, u32 c, u32 next_c, VTState state, Optional<Style::Mask> const& mask, Optional<size_t> const& maximum_line_width = {}, Optional<size_t&> last_return = {});

enum class MaskedSelectionDecision {
    Skip,
    Continue,
};
static MaskedSelectionDecision resolve_masked_selection(Optional<Style::Mask>& mask, size_t& i, auto& mask_it, auto& view, auto& state, auto& metrics, auto& current_line)
{
    if (mask.has_value() && mask->mode == Style::Mask::Mode::ReplaceEntireSelection) {
        ++mask_it;
        auto actual_end_offset = mask_it.is_end() ? view.length() : mask_it.key();
        auto end_offset = min(actual_end_offset, view.length());
        size_t j = 0;
        for (auto it = mask->replacement_view.begin(); it != mask->replacement_view.end(); ++it) {
            auto it_copy = it;
            ++it_copy;
            auto next_c = it_copy == mask->replacement_view.end() ? 0 : *it_copy;
            state = actual_rendered_string_length_step(metrics, j, current_line, *it, next_c, state, {});
            ++j;
            if (j <= actual_end_offset - i && j + i >= view.length())
                break;
        }
        current_line.masked_chars.empend(i, end_offset - i, j);
        i = end_offset;

        if (mask_it.is_end())
            mask = {};
        else
            mask = *mask_it;
        return MaskedSelectionDecision::Skip;
    }
    return MaskedSelectionDecision::Continue;
}

StringMetrics Editor::actual_rendered_string_metrics(StringView string, RedBlackTree<u32, Optional<Style::Mask>> const& masks, Optional<size_t> maximum_line_width)
{
    Vector<u32> utf32_buffer;
    utf32_buffer.ensure_capacity(string.length());
    for (auto c : Utf8View { string })
        utf32_buffer.append(c);

    return actual_rendered_string_metrics(Utf32View { utf32_buffer.data(), utf32_buffer.size() }, masks, maximum_line_width);
}

StringMetrics Editor::actual_rendered_string_metrics(Utf32View const& view, RedBlackTree<u32, Optional<Style::Mask>> const& masks, Optional<size_t> maximum_line_width)
{
    StringMetrics metrics;
    StringMetrics::LineMetrics current_line;
    VTState state { Free };
    Optional<Style::Mask> mask;
    size_t last_return { 0 };

    auto mask_it = masks.begin();

    auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Grapheme);
    Vector<size_t> grapheme_breaks;

    segmenter->for_each_boundary(view, [&](size_t offset) -> IterationDecision {
        if (offset >= view.length())
            return IterationDecision::Break;

        grapheme_breaks.append(offset);
        return IterationDecision::Continue;
    });

    for (size_t break_index = 0; break_index < grapheme_breaks.size(); ++break_index) {
        auto i = grapheme_breaks[break_index];
        auto c = view[i];
        if (!mask_it.is_end() && mask_it.key() <= i)
            mask = *mask_it;

        if (resolve_masked_selection(mask, i, mask_it, view, state, metrics, current_line) == MaskedSelectionDecision::Skip) {
            --i;
            binary_search(grapheme_breaks, i, &break_index);
            continue;
        }

        auto next_c = break_index + 1 < grapheme_breaks.size() ? view.code_points()[grapheme_breaks[break_index + 1]] : 0;
        state = actual_rendered_string_length_step(metrics, i, current_line, c, next_c, state, mask, maximum_line_width, last_return);
        if (!mask_it.is_end() && mask_it.key() <= i) {
            auto mask_it_peek = mask_it;
            ++mask_it_peek;
            if (!mask_it_peek.is_end() && mask_it_peek.key() > i)
                mask_it = mask_it_peek;
        }
    }

    metrics.line_metrics.append(current_line);

    for (auto& line : metrics.line_metrics)
        metrics.max_line_length = max(line.total_length(), metrics.max_line_length);

    metrics.grapheme_breaks = move(grapheme_breaks);

    return metrics;
}

VTState actual_rendered_string_length_step(StringMetrics& metrics, size_t index, StringMetrics::LineMetrics& current_line, u32 c, u32 next_c, VTState state, Optional<Style::Mask> const& mask, Optional<size_t> const& maximum_line_width, Optional<size_t&> last_return)
{
    auto const save_line = [&metrics, &current_line, &last_return, &index]() {
        if (last_return.has_value()) {
            auto const last_index = index - 1;
            current_line.bit_length = last_index - *last_return + 1;
            last_return.value() = last_index + 1;
        }
        metrics.line_metrics.append(current_line);

        current_line.masked_chars = {};
        current_line.length = 0;
        current_line.visible_length = 0;
        current_line.bit_length = {};
    };

    // FIXME: current_line.visible_length can go above maximum_line_width when using masks
    if (maximum_line_width.has_value() && current_line.visible_length >= maximum_line_width.value())
        save_line();

    ScopeGuard bit_length_update { [&last_return, &current_line, &index]() {
        if (last_return.has_value())
            current_line.bit_length = index - *last_return + 1;
    } };

    switch (state) {
    case Free: {
        if (c == '\x1b') { // escape
            return Escape;
        }
        if (c == '\r') { // carriage return
            current_line.masked_chars = {};
            current_line.length = 0;
            current_line.visible_length = 0;
            if (!metrics.line_metrics.is_empty())
                metrics.line_metrics.last() = { {}, 0 };
            return state;
        }
        if (c == '\n') { // return
            save_line();
            return state;
        }
        if (c == '\t') {
            // Tabs are a special case, because their width is variable.
            ++current_line.length;
            current_line.visible_length += (8 - (current_line.visible_length % 8));
            return state;
        }
        auto is_control = is_ascii_control(c);
        if (is_control) {
            if (mask.has_value())
                current_line.masked_chars.append({ index, 1, mask->replacement_view.length() });
            else
                current_line.masked_chars.append({ index, 1, c < 64 ? 2u : 4u }); // if the character cannot be represented as ^c, represent it as \xbb.
        }
        // FIXME: This will not support anything sophisticated
        if (mask.has_value()) {
            current_line.length += mask->replacement_view.length();
            current_line.visible_length += mask->replacement_view.length();
            metrics.total_length += mask->replacement_view.length();
        } else if (is_control) {
            current_line.length += current_line.masked_chars.last().masked_length;
            current_line.visible_length += current_line.masked_chars.last().masked_length;
            metrics.total_length += current_line.masked_chars.last().masked_length;
        } else {
            ++current_line.length;
            ++current_line.visible_length;
            ++metrics.total_length;
        }
        return state;
    }
    case Escape:
        if (c == ']') {
            if (next_c == '0')
                state = Title;
            if (next_c == '8')
                state = URL;
            return state;
        }
        if (c == '[') {
            return Bracket;
        }
        // FIXME: This does not support non-VT (aside from set-title) escapes
        return state;
    case Bracket:
        if (is_ascii_digit(c)) {
            return BracketArgsSemi;
        }
        return state;
    case BracketArgsSemi:
        if (c == ';') {
            return Bracket;
        }
        if (!is_ascii_digit(c))
            state = Free;
        return state;
    case Title:
        if (c == 7)
            state = Free;
        return state;
    case URL:
        if (c == '\\')
            state = Free;
        return state;
    }
    return state;
}

Result<Vector<size_t, 2>, Editor::Error> Editor::vt_dsr()
{
    char buf[16];

    // Read whatever junk there is before talking to the terminal
    // and insert them later when we're reading user input.
    bool more_junk_to_read { false };
    timeval timeout { 0, 0 };
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(0, &readfds);

    do {
        more_junk_to_read = false;
        [[maybe_unused]] auto rc = select(1, &readfds, nullptr, nullptr, &timeout);
        if (FD_ISSET(0, &readfds)) {
            auto nread = read(0, buf, 16);
            if (nread < 0) {
                m_input_error = Error::ReadFailure;
                finish();
                break;
            }

            if (nread == 0)
                break;

            m_incomplete_data.append(buf, nread);
            more_junk_to_read = true;
        }
    } while (more_junk_to_read);

    if (m_input_error.has_value())
        return m_input_error.value();

    fputs("\033[6n", stderr);
    fflush(stderr);

    // Parse the DSR response
    // it should be of the form .*\e[\d+;\d+R.*
    // Anything not part of the response is just added to the incomplete data.
    enum {
        Free,
        SawEsc,
        SawBracket,
        InFirstCoordinate,
        SawSemicolon,
        InSecondCoordinate,
        SawR,
    } state { Free };
    auto has_error = false;
    Vector<char, 4> coordinate_buffer;
    size_t row { 1 }, col { 1 };

    do {
        char c;
        auto nread = read(0, &c, 1);
        if (nread < 0) {
            if (errno == 0 || errno == EINTR) {
                // ????
                continue;
            }
            dbgln("Error while reading DSR: {}", strerror(errno));
            return Error::ReadFailure;
        }
        if (nread == 0) {
            dbgln("Terminal DSR issue; received no response");
            return Error::Empty;
        }

        switch (state) {
        case Free:
            if (c == '\x1b') {
                state = SawEsc;
                continue;
            }
            m_incomplete_data.append(c);
            continue;
        case SawEsc:
            if (c == '[') {
                state = SawBracket;
                continue;
            }
            m_incomplete_data.append(c);
            state = Free;
            continue;
        case SawBracket:
            if (is_ascii_digit(c)) {
                state = InFirstCoordinate;
                coordinate_buffer.clear_with_capacity();
                coordinate_buffer.append(c);
                continue;
            }
            m_incomplete_data.append(c);
            state = Free;
            continue;
        case InFirstCoordinate:
            if (is_ascii_digit(c)) {
                coordinate_buffer.append(c);
                continue;
            }
            if (c == ';') {
                auto maybe_row = StringView { coordinate_buffer.data(), coordinate_buffer.size() }.to_number<unsigned>();
                if (!maybe_row.has_value())
                    has_error = true;
                row = maybe_row.value_or(1u);
                coordinate_buffer.clear_with_capacity();
                state = SawSemicolon;
                continue;
            }
            m_incomplete_data.append(c);
            state = Free;
            continue;
        case SawSemicolon:
            if (is_ascii_digit(c)) {
                state = InSecondCoordinate;
                coordinate_buffer.append(c);
                continue;
            }
            m_incomplete_data.append(c);
            state = Free;
            continue;
        case InSecondCoordinate:
            if (is_ascii_digit(c)) {
                coordinate_buffer.append(c);
                continue;
            }
            if (c == 'R') {
                auto maybe_column = StringView { coordinate_buffer.data(), coordinate_buffer.size() }.to_number<unsigned>();
                if (!maybe_column.has_value())
                    has_error = true;
                col = maybe_column.value_or(1u);
                coordinate_buffer.clear_with_capacity();
                state = SawR;
                continue;
            }
            m_incomplete_data.append(c);
            state = Free;
            continue;
        case SawR:
            m_incomplete_data.append(c);
            continue;
        default:
            VERIFY_NOT_REACHED();
        }
    } while (state != SawR);

    if (has_error)
        dbgln("Terminal DSR issue, couldn't parse DSR response");
    return Vector<size_t, 2> { row, col };
}

ByteString Editor::line(size_t up_to_index) const
{
    StringBuilder builder;
    builder.append(Utf32View { m_buffer.data(), min(m_buffer.size(), up_to_index) });
    return builder.to_byte_string();
}

void Editor::remove_at_index(size_t index)
{
    // See if we have any anchored styles, and reposition them if needed.
    readjust_anchored_styles(index, ModificationKind::Removal);
    auto cp = m_buffer[index];
    m_buffer.remove(index);
    if (cp == '\n')
        ++m_extra_forward_lines;
    ++m_chars_touched_in_the_middle;
}

void Editor::readjust_anchored_styles(size_t hint_index, ModificationKind modification)
{
    struct Anchor {
        Span old_span;
        Span new_span;
        Style style;
    };
    Vector<Anchor> anchors_to_relocate;
    auto index_shift = modification == ModificationKind::Insertion ? 1 : -1;
    auto forced_removal = modification == ModificationKind::ForcedOverlapRemoval;

    for (auto& start_entry : m_current_spans.m_anchored_spans_starting) {
        for (auto& end_entry : start_entry.value) {
            if (forced_removal) {
                if (start_entry.key <= hint_index && end_entry.key > hint_index) {
                    // Remove any overlapping regions.
                    continue;
                }
            }
            if (start_entry.key >= hint_index) {
                if (start_entry.key == hint_index && end_entry.key == hint_index + 1 && modification == ModificationKind::Removal) {
                    // Remove the anchor, as all its text was wiped.
                    continue;
                }
                // Shift everything.
                anchors_to_relocate.append({ { start_entry.key, end_entry.key, Span::Mode::CodepointOriented }, { start_entry.key + index_shift, end_entry.key + index_shift, Span::Mode::CodepointOriented }, end_entry.value });
                continue;
            }
            if (end_entry.key > hint_index) {
                // Shift just the end.
                anchors_to_relocate.append({ { start_entry.key, end_entry.key, Span::Mode::CodepointOriented }, { start_entry.key, end_entry.key + index_shift, Span::Mode::CodepointOriented }, end_entry.value });
                continue;
            }
            anchors_to_relocate.append({ { start_entry.key, end_entry.key, Span::Mode::CodepointOriented }, { start_entry.key, end_entry.key, Span::Mode::CodepointOriented }, end_entry.value });
        }
    }

    m_current_spans.m_anchored_spans_ending.clear();
    m_current_spans.m_anchored_spans_starting.clear();
    // Pass over the relocations and update the stale entries.
    for (auto& relocation : anchors_to_relocate) {
        stylize(relocation.new_span, relocation.style);
    }
}

size_t StringMetrics::lines_with_addition(StringMetrics const& offset, size_t column_width) const
{
    size_t lines = 0;

    if (!line_metrics.is_empty()) {
        for (size_t i = 0; i < line_metrics.size() - 1; ++i)
            lines += (line_metrics[i].total_length() + column_width) / column_width;

        auto last = line_metrics.last().total_length();
        last += offset.line_metrics.first().total_length();
        lines += (last + column_width) / column_width;
    }

    for (size_t i = 1; i < offset.line_metrics.size(); ++i)
        lines += (offset.line_metrics[i].total_length() + column_width) / column_width;

    return lines;
}

size_t StringMetrics::offset_with_addition(StringMetrics const& offset, size_t column_width) const
{
    if (offset.line_metrics.size() > 1)
        return offset.line_metrics.last().total_length() % column_width;

    if (!line_metrics.is_empty()) {
        auto last = line_metrics.last().total_length();
        last += offset.line_metrics.first().total_length();
        return last % column_width;
    }

    if (offset.line_metrics.is_empty())
        return 0;

    return offset.line_metrics.first().total_length() % column_width;
}

bool Editor::Spans::contains_up_to_offset(Spans const& other, size_t offset) const
{
    auto compare = [&]<typename K, typename V>(HashMap<K, HashMap<K, V>> const& left, HashMap<K, HashMap<K, V>> const& right) -> bool {
        for (auto& entry : right) {
            if (entry.key > offset + 1)
                continue;

            auto left_map_it = left.find(entry.key);
            if (left_map_it == left.end())
                return false;

            for (auto& left_entry : left_map_it->value) {
                auto value_it = entry.value.find(left_entry.key);
                if (value_it == entry.value.end()) {
                    // Might have the same thing with a longer span
                    bool found = false;
                    for (auto& possibly_longer_span_entry : entry.value) {
                        if (possibly_longer_span_entry.key > left_entry.key && possibly_longer_span_entry.key > offset && left_entry.value == possibly_longer_span_entry.value) {
                            found = true;
                            break;
                        }
                    }
                    if (found)
                        continue;
                    if constexpr (LINE_EDITOR_DEBUG) {
                        dbgln("Compare for {}-{} failed, no entry", entry.key, left_entry.key);
                        for (auto& x : entry.value)
                            dbgln("Have: {}-{} = {}", entry.key, x.key, x.value.to_byte_string());
                    }
                    return false;
                } else if (value_it->value != left_entry.value) {
                    dbgln_if(LINE_EDITOR_DEBUG, "Compare for {}-{} failed, different values: {} != {}", entry.key, left_entry.key, value_it->value.to_byte_string(), left_entry.value.to_byte_string());
                    return false;
                }
            }
        }

        return true;
    };

    return compare(m_spans_starting, other.m_spans_starting)
        && compare(m_anchored_spans_starting, other.m_anchored_spans_starting);
}

}
