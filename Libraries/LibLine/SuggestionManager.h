/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Forward.h>
#include <AK/String.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>
#include <LibLine/Style.h>

namespace Line {

struct CompletionSuggestion {
private:
    struct ForSearchTag {
    };

public:
    static constexpr ForSearchTag ForSearch {};

    // Intentionally not explicit. (To allow suggesting bare strings)
    CompletionSuggestion(ByteString const& completion)
        : CompletionSuggestion(completion, ""sv, {})
    {
    }

    CompletionSuggestion(StringView completion, ForSearchTag)
        : text(MUST(String::from_utf8(completion)))
    {
    }

    CompletionSuggestion(StringView completion, StringView trailing_trivia, StringView display_trivia = ""sv)
        : CompletionSuggestion(completion, trailing_trivia, display_trivia, {})
    {
    }

    CompletionSuggestion(StringView completion, StringView trailing_trivia, StringView display_trivia, Style style);

    bool operator==(CompletionSuggestion const& suggestion) const
    {
        return suggestion.text == text;
    }

    String text;
    String trailing_trivia;
    String display_trivia;
    Style style;
    size_t start_index { 0 };
    size_t input_offset { 0 };
    size_t static_offset { 0 };
    size_t invariant_offset { 0 };
    bool allow_commit_without_listing { true };

    Utf8View text_view() const { return text.code_points(); }
    Utf8View trivia_view() const { return trailing_trivia.code_points(); }
    Utf8View display_trivia_view() const { return display_trivia.code_points(); }
    StringView text_string() const { return text.bytes_as_string_view(); }
    StringView display_trivia_string() const { return display_trivia.bytes_as_string_view(); }
    bool is_valid { false };
};

class SuggestionManager {
    friend class Editor;

public:
    void set_suggestions(Vector<CompletionSuggestion>&& suggestions);
    void set_current_suggestion_initiation_index(size_t start_index);

    size_t count() const { return m_suggestions.size(); }
    size_t display_length() const { return m_last_shown_suggestion_display_length; }
    size_t start_index() const { return m_last_displayed_suggestion_index; }
    size_t next_index() const { return m_next_suggestion_index; }
    void set_start_index(size_t index) const { m_last_displayed_suggestion_index = index; }

    ErrorOr<size_t> for_each_suggestion(Function<ErrorOr<IterationDecision>(CompletionSuggestion const&, size_t)>) const;

    enum CompletionMode {
        DontComplete,
        CompletePrefix,
        ShowSuggestions,
        CycleSuggestions,
    };

    class CompletionAttemptResult {
    public:
        CompletionMode new_completion_mode;

        ssize_t new_cursor_offset { 0 };

        struct {
            size_t start;
            size_t end;
        } offset_region_to_remove { 0, 0 }; // The region to remove as defined by [start, end) translated by (old_cursor + new_cursor_offset)

        // This bit of data will be removed, but restored if the suggestion is rejected.
        size_t static_offset_from_cursor { 0 };

        Vector<Utf8View> insert {};

        Optional<Style> style_to_apply {};

        bool avoid_committing_to_single_suggestion { false };
    };

    CompletionAttemptResult attempt_completion(CompletionMode, size_t initiation_start_index);

    void next();
    void previous();

    CompletionSuggestion const& suggest();
    CompletionSuggestion const& current_suggestion() const { return m_last_shown_suggestion; }
    bool is_current_suggestion_complete() const { return m_last_shown_suggestion_was_complete; }

    void reset()
    {
        m_last_shown_suggestion = ByteString::empty();
        m_last_shown_suggestion_display_length = 0;
        m_suggestions.clear();
        m_last_displayed_suggestion_index = 0;
        m_next_suggestion_index = 0;
    }

private:
    SuggestionManager()
    {
    }

    Vector<CompletionSuggestion> m_suggestions;
    CompletionSuggestion m_last_shown_suggestion { ByteString::empty() };
    size_t m_last_shown_suggestion_display_length { 0 };
    bool m_last_shown_suggestion_was_complete { false };
    mutable size_t m_next_suggestion_index { 0 };
    size_t m_largest_common_suggestion_prefix_length { 0 };
    mutable size_t m_last_displayed_suggestion_index { 0 };
    size_t m_selected_suggestion_index { 0 };
};

}
