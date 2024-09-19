/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BinarySearch.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/RedBlackTree.h>
#include <AK/Result.h>
#include <AK/Traits.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibCore/DirIterator.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibLine/KeyCallbackMachine.h>
#include <LibLine/Span.h>
#include <LibLine/StringMetrics.h>
#include <LibLine/Style.h>
#include <LibLine/SuggestionDisplay.h>
#include <LibLine/SuggestionManager.h>
#include <LibLine/VT.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>

namespace Line {

static constexpr u32 ctrl(char c) { return c & 0x3f; }

struct KeyBinding {
    Vector<Key> keys;
    enum class Kind {
        InternalFunction,
        Insertion,
    } kind { Kind::InternalFunction };
    ByteString binding;
};

struct Configuration {
    enum RefreshBehavior {
        Lazy,
        Eager,
    };
    enum OperationMode {
        Unset,
        Full,
        NoEscapeSequences,
        NonInteractive,
    };
    enum SignalHandler {
        WithSignalHandlers,
        NoSignalHandlers,
    };

    enum Flags : u32 {
        None = 0,
        BracketedPaste = 1,
    };

    struct DefaultTextEditor {
        ByteString command;
    };

    Configuration()
    {
    }

    template<typename Arg, typename... Rest>
    Configuration(Arg arg, Rest... rest)
        : Configuration(rest...)
    {
        set(arg);
    }

    void set(RefreshBehavior refresh) { refresh_behavior = refresh; }
    void set(OperationMode mode) { operation_mode = mode; }
    void set(SignalHandler mode) { m_signal_mode = mode; }
    void set(KeyBinding const& binding) { keybindings.append(binding); }
    void set(DefaultTextEditor editor) { m_default_text_editor = move(editor.command); }
    void set(Flags flags)
    {
        enable_bracketed_paste = flags & Flags::BracketedPaste;
    }

    static Configuration from_config(StringView libname = "line"sv);

    RefreshBehavior refresh_behavior { RefreshBehavior::Lazy };
    SignalHandler m_signal_mode { SignalHandler::WithSignalHandlers };
    OperationMode operation_mode { OperationMode::Unset };
    Vector<KeyBinding> keybindings;
    ByteString m_default_text_editor {};
    bool enable_bracketed_paste { false };
};

#define ENUMERATE_EDITOR_INTERNAL_FUNCTIONS(M) \
    M(clear_screen)                            \
    M(cursor_left_character)                   \
    M(cursor_left_word)                        \
    M(cursor_left_nonspace_word)               \
    M(cursor_right_character)                  \
    M(cursor_right_word)                       \
    M(cursor_right_nonspace_word)              \
    M(enter_search)                            \
    M(search_character_backwards)              \
    M(search_character_forwards)               \
    M(erase_character_backwards)               \
    M(erase_character_forwards)                \
    M(erase_to_beginning)                      \
    M(erase_to_end)                            \
    M(erase_word_backwards)                    \
    M(finish_edit)                             \
    M(go_end)                                  \
    M(go_home)                                 \
    M(kill_line)                               \
    M(search_backwards)                        \
    M(search_forwards)                         \
    M(transpose_characters)                    \
    M(transpose_words)                         \
    M(insert_last_words)                       \
    M(insert_last_erased)                      \
    M(erase_alnum_word_backwards)              \
    M(erase_alnum_word_forwards)               \
    M(erase_spaces)                            \
    M(capitalize_word)                         \
    M(lowercase_word)                          \
    M(uppercase_word)                          \
    M(edit_in_external_editor)

#define EDITOR_INTERNAL_FUNCTION(name) \
    [](auto& editor) { editor.name();  return false; }

class Editor : public Core::EventReceiver {
    C_OBJECT(Editor);

public:
    enum class Error {
        ReadFailure,
        Empty,
        Eof,
    };

    ~Editor();

    Result<ByteString, Error> get_line(ByteString const& prompt);

    void initialize();

    void refetch_default_termios();

    void add_to_history(ByteString const& line);
    bool load_history(ByteString const& path);
    bool save_history(ByteString const& path);
    auto const& history() const { return m_history; }
    bool is_history_dirty() const { return m_history_dirty; }

    void register_key_input_callback(KeyBinding const&);
    void register_key_input_callback(Vector<Key> keys, Function<bool(Editor&)> callback) { m_callback_machine.register_key_input_callback(move(keys), move(callback)); }
    void register_key_input_callback(Key key, Function<bool(Editor&)> callback) { register_key_input_callback(Vector<Key> { key }, move(callback)); }

    static StringMetrics actual_rendered_string_metrics(StringView, RedBlackTree<u32, Optional<Style::Mask>> const& masks = {}, Optional<size_t> maximum_line_width = {});
    static StringMetrics actual_rendered_string_metrics(Utf32View const&, RedBlackTree<u32, Optional<Style::Mask>> const& masks = {}, Optional<size_t> maximum_line_width = {});

    Function<Vector<CompletionSuggestion>(Editor const&)> on_tab_complete;
    Function<void(Utf32View, Editor&)> on_paste;
    Function<void()> on_interrupt_handled;
    Function<void(Editor&)> on_display_refresh;

    static Function<bool(Editor&)> find_internal_function(StringView name);
    enum class CaseChangeOp {
        Lowercase,
        Uppercase,
        Capital,
    };
    void case_change_word(CaseChangeOp);
#define __ENUMERATE_EDITOR_INTERNAL_FUNCTION(name) \
    void name();

    ENUMERATE_EDITOR_INTERNAL_FUNCTIONS(__ENUMERATE_EDITOR_INTERNAL_FUNCTION)

#undef __ENUMERATE_EDITOR_INTERNAL_FUNCTION

    ErrorOr<void> interrupted();
    ErrorOr<void> resized();

    size_t cursor() const { return m_cursor; }
    void set_cursor(size_t cursor)
    {
        if (cursor > m_buffer.size())
            cursor = m_buffer.size();
        m_cursor = cursor;
    }
    Vector<u32, 1024> const& buffer() const { return m_buffer; }
    u32 buffer_at(size_t pos) const { return m_buffer.at(pos); }
    ByteString line() const { return line(m_buffer.size()); }
    ByteString line(size_t up_to_index) const;

    // Only makes sense inside a character_input callback or on_* callback.
    void set_prompt(ByteString const& prompt)
    {
        if (m_cached_prompt_valid)
            m_old_prompt_metrics = m_cached_prompt_metrics;
        m_cached_prompt_valid = false;
        m_cached_prompt_metrics = actual_rendered_string_metrics(prompt, {});
        m_new_prompt = prompt;
    }

    void clear_line();
    void insert(ByteString const&);
    void insert(StringView);
    void insert(Utf8View&);
    void insert(Utf32View const&);
    void insert(u32 const);
    void stylize(Span const&, Style const&);
    void strip_styles(bool strip_anchored = false);

    // Invariant Offset is an offset into the suggested data, hinting the editor what parts of the suggestion will not change
    // Static Offset is an offset into the token, signifying where the suggestions start
    // e.g.
    //    foobar<suggestion initiated>, on_tab_complete returns "barx", "bary", "barz"
    //       ^ ^
    //       +-|- static offset: the suggestions start here
    //         +- invariant offset: the suggestions do not change up to here
    //
    void transform_suggestion_offsets(size_t& invariant_offset, size_t& static_offset, Span::Mode offset_mode = Span::ByteOriented) const;

    const struct termios& termios() const { return m_termios; }
    const struct termios& default_termios() const { return m_default_termios; }
    struct winsize terminal_size() const
    {
        winsize ws { (u16)m_num_lines, (u16)m_num_columns, 0, 0 };
        return ws;
    }

    void finish()
    {
        m_finish = true;
    }

    bool is_editing() const { return m_is_editing; }

    Utf32View const buffer_view() const { return { m_buffer.data(), m_buffer.size() }; }

    auto prohibit_input()
    {
        auto previous_value = m_prohibit_input_processing;
        m_prohibit_input_processing = true;
        m_have_unprocessed_read_event = false;
        return ScopeGuard {
            [this, previous_value] {
                m_prohibit_input_processing = previous_value;
                if (!m_prohibit_input_processing && m_have_unprocessed_read_event)
                    handle_read_event().release_value_but_fixme_should_propagate_errors();
            }
        };
    }

private:
    explicit Editor(Configuration configuration = Configuration::from_config());

    void set_default_keybinds();

    enum LoopExitCode {
        Exit = 0,
        Retry
    };

    ErrorOr<void> try_update_once();
    void handle_interrupt_event();
    ErrorOr<void> handle_read_event();
    ErrorOr<void> handle_resize_event(bool reset_origin);

    void ensure_free_lines_from_origin(size_t count);

    Result<Vector<size_t, 2>, Error> vt_dsr();
    void remove_at_index(size_t);

    enum class ModificationKind {
        Insertion,
        Removal,
        ForcedOverlapRemoval,
    };
    void readjust_anchored_styles(size_t hint_index, ModificationKind);

    Style find_applicable_style(size_t offset) const;

    bool search(StringView, bool allow_empty = false, bool from_beginning = true);
    inline void end_search()
    {
        m_is_searching = false;
        m_refresh_needed = true;
        m_search_offset = 0;
        if (m_reset_buffer_on_search_end) {
            m_buffer.clear();
            for (auto ch : m_pre_search_buffer)
                m_buffer.append(ch);
            m_cursor = m_pre_search_cursor;
        }
        m_reset_buffer_on_search_end = true;
        m_search_editor = nullptr;
    }

    void reset()
    {
        m_cached_buffer_metrics.reset();
        m_cached_prompt_valid = false;
        m_cursor = 0;
        m_drawn_cursor = 0;
        m_inline_search_cursor = 0;
        m_search_offset = 0;
        m_search_offset_state = SearchOffsetState::Unbiased;
        m_old_prompt_metrics = m_cached_prompt_metrics;
        set_origin(0, 0);
        m_prompt_lines_at_suggestion_initiation = 0;
        m_refresh_needed = true;
        m_input_error.clear();
        m_returned_line = ByteString::empty();
        m_chars_touched_in_the_middle = 0;
        m_drawn_end_of_line_offset = 0;
        m_drawn_spans = {};
        m_paste_buffer.clear_with_capacity();
    }

    ErrorOr<void> refresh_display();
    ErrorOr<void> cleanup();
    ErrorOr<void> cleanup_suggestions();
    ErrorOr<void> really_quit_event_loop();

    void restore()
    {
        VERIFY(m_initialized);
        tcsetattr(0, TCSANOW, &m_default_termios);
        m_initialized = false;
        if (m_configuration.enable_bracketed_paste)
            warn("\x1b[?2004l");
        for (auto id : m_signal_handlers)
            Core::EventLoop::unregister_signal(id);
    }

    StringMetrics const& current_prompt_metrics() const
    {
        return m_cached_prompt_valid ? m_cached_prompt_metrics : m_old_prompt_metrics;
    }

    size_t num_lines() const
    {
        return current_prompt_metrics().lines_with_addition(m_cached_buffer_metrics, m_num_columns);
    }

    size_t cursor_line() const
    {
        auto cursor = m_drawn_cursor;
        if (cursor > m_cursor)
            cursor = m_cursor;
        return current_prompt_metrics().lines_with_addition(
            actual_rendered_string_metrics(buffer_view().substring_view(0, cursor), m_current_masks),
            m_num_columns);
    }

    size_t offset_in_line() const
    {
        auto cursor = m_drawn_cursor;
        if (cursor > m_cursor)
            cursor = m_cursor;
        auto buffer_metrics = actual_rendered_string_metrics(buffer_view().substring_view(0, cursor), m_current_masks);
        return current_prompt_metrics().offset_with_addition(buffer_metrics, m_num_columns);
    }

    bool set_origin(bool quit_on_error = true)
    {
        auto position = vt_dsr();
        if (!position.is_error()) {
            set_origin(position.value()[0], position.value()[1]);
            return true;
        }
        if (quit_on_error && position.is_error()) {
            m_input_error = position.error();
            finish();
        }
        return false;
    }

    void set_origin(int row, int col)
    {
        m_origin_row = row;
        m_origin_column = col;
        m_suggestion_display->set_origin(row, col, {});
    }

    void recalculate_origin();
    ErrorOr<void> reposition_cursor(Stream&, bool to_end = false);

    struct CodepointRange {
        size_t start { 0 };
        size_t end { 0 };
    };
    CodepointRange byte_offset_range_to_code_point_offset_range(size_t byte_start, size_t byte_end, size_t code_point_scan_offset, bool reverse = false) const;

    void get_terminal_size();

    bool m_finish { false };

    RefPtr<Editor> m_search_editor;
    bool m_is_searching { false };
    bool m_reset_buffer_on_search_end { true };
    size_t m_search_offset { 0 };
    enum class SearchOffsetState {
        Unbiased,
        Backwards,
        Forwards,
    } m_search_offset_state { SearchOffsetState::Unbiased };
    size_t m_pre_search_cursor { 0 };
    Vector<u32, 1024> m_pre_search_buffer;

    Vector<u32, 1024> m_buffer;
    ByteBuffer m_pending_chars;
    Vector<char, 512> m_incomplete_data;
    Optional<Error> m_input_error;
    ByteString m_returned_line;

    size_t m_cursor { 0 };
    size_t m_drawn_cursor { 0 };
    size_t m_drawn_end_of_line_offset { 0 };
    size_t m_inline_search_cursor { 0 };
    size_t m_chars_touched_in_the_middle { 0 };
    size_t m_times_tab_pressed { 0 };
    size_t m_num_columns { 0 };
    size_t m_num_lines { 1 };
    size_t m_previous_num_columns { 0 };
    size_t m_extra_forward_lines { 0 };
    size_t m_shown_lines { 0 };
    StringMetrics m_cached_prompt_metrics;
    StringMetrics m_old_prompt_metrics;
    StringMetrics m_cached_buffer_metrics;
    size_t m_prompt_lines_at_suggestion_initiation { 0 };
    bool m_cached_prompt_valid { false };

    // Exact position before our prompt in the terminal.
    size_t m_origin_row { 0 };
    size_t m_origin_column { 0 };
    bool m_expected_origin_changed { false };
    bool m_has_origin_reset_scheduled { false };

    OwnPtr<SuggestionDisplay> m_suggestion_display;
    Vector<u32, 32> m_remembered_suggestion_static_data;

    ByteString m_new_prompt;

    SuggestionManager m_suggestion_manager;

    bool m_always_refresh { false };

    enum class TabDirection {
        Forward,
        Backward,
    };
    TabDirection m_tab_direction { TabDirection::Forward };

    KeyCallbackMachine m_callback_machine;

    struct termios m_termios {
    };
    struct termios m_default_termios {
    };
    bool m_was_interrupted { false };
    bool m_previous_interrupt_was_handled_as_interrupt { true };
    bool m_was_resized { false };

    // FIXME: This should be something more take_first()-friendly.
    struct HistoryEntry {
        ByteString entry;
        time_t timestamp;
    };
    Vector<HistoryEntry> m_history;
    size_t m_history_cursor { 0 };
    size_t m_history_capacity { 1024 };
    bool m_history_dirty { false };
    static ErrorOr<Vector<HistoryEntry>> try_load_history(StringView path);

    enum class InputState {
        Free,
        Verbatim,
        Paste,
        GotEscape,
        CSIExpectParameter,
        CSIExpectIntermediate,
        CSIExpectFinal,
    };
    InputState m_state { InputState::Free };
    InputState m_previous_free_state { InputState::Free };

    struct Spans {
        HashMap<u32, HashMap<u32, Style>> m_spans_starting;
        HashMap<u32, HashMap<u32, Style>> m_spans_ending;
        HashMap<u32, HashMap<u32, Style>> m_anchored_spans_starting;
        HashMap<u32, HashMap<u32, Style>> m_anchored_spans_ending;

        bool contains_up_to_offset(Spans const& other, size_t offset) const;
    } m_drawn_spans, m_current_spans;

    RedBlackTree<u32, Optional<Style::Mask>> m_current_masks;

    RefPtr<Core::Notifier> m_notifier;

    Vector<u32> m_paste_buffer;
    Vector<u32> m_last_erased;

    bool m_initialized { false };
    bool m_refresh_needed { false };
    Vector<int, 2> m_signal_handlers;

    bool m_is_editing { false };
    bool m_prohibit_input_processing { false };
    bool m_have_unprocessed_read_event { false };

    Configuration m_configuration;
};

}
