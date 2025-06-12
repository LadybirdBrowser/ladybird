/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Function.h>
#include <LibLine/SuggestionManager.h>

namespace Line {

CompletionSuggestion::CompletionSuggestion(StringView completion, StringView trailing_trivia, StringView display_trivia, Style style)
    : text(MUST(String::from_utf8(completion)))
    , trailing_trivia(MUST(String::from_utf8(trailing_trivia)))
    , display_trivia(MUST(String::from_utf8(display_trivia)))
    , style(style)
    , is_valid(true)
{
}

void SuggestionManager::set_suggestions(Vector<CompletionSuggestion>&& suggestions)
{
    auto code_point_at = [](Utf8View view, size_t index) {
        size_t count = 0;
        for (auto cp : view) {
            if (count == index) {
                return cp;
            }
            count++;
        }
        VERIFY_NOT_REACHED();
    };

    m_suggestions = move(suggestions);

    size_t common_suggestion_prefix { 0 };
    if (m_suggestions.size() == 1) {
        m_largest_common_suggestion_prefix_length = m_suggestions[0].text_view().length();
    } else if (m_suggestions.size()) {
        u32 last_valid_suggestion_code_point;

        for (;; ++common_suggestion_prefix) {
            if (m_suggestions[0].text_view().length() <= common_suggestion_prefix)
                goto no_more_commons;

            last_valid_suggestion_code_point = code_point_at(m_suggestions[0].text_view(), common_suggestion_prefix);

            for (auto& suggestion : m_suggestions) {
                if (suggestion.text_view().length() <= common_suggestion_prefix || code_point_at(suggestion.text_view(), common_suggestion_prefix) != last_valid_suggestion_code_point) {
                    goto no_more_commons;
                }
            }
        }
    no_more_commons:;
        m_largest_common_suggestion_prefix_length = common_suggestion_prefix;
    } else {
        m_largest_common_suggestion_prefix_length = 0;
    }
}

void SuggestionManager::next()
{
    if (m_suggestions.size())
        m_next_suggestion_index = (m_next_suggestion_index + 1) % m_suggestions.size();
    else
        m_next_suggestion_index = 0;
}

void SuggestionManager::previous()
{
    if (m_next_suggestion_index == 0)
        m_next_suggestion_index = m_suggestions.size();
    m_next_suggestion_index--;
}

CompletionSuggestion const& SuggestionManager::suggest()
{
    auto const& suggestion = m_suggestions[m_next_suggestion_index];
    m_selected_suggestion_index = m_next_suggestion_index;
    m_last_shown_suggestion = suggestion;
    return suggestion;
}

void SuggestionManager::set_current_suggestion_initiation_index(size_t index)
{
    auto& suggestion = m_suggestions[m_next_suggestion_index];

    if (m_last_shown_suggestion_display_length)
        m_last_shown_suggestion.start_index = index - suggestion.static_offset - m_last_shown_suggestion_display_length;
    else
        m_last_shown_suggestion.start_index = index - suggestion.static_offset - suggestion.invariant_offset;

    m_last_shown_suggestion_display_length = m_last_shown_suggestion.text_view().length();
    m_last_shown_suggestion_was_complete = true;
}

SuggestionManager::CompletionAttemptResult SuggestionManager::attempt_completion(CompletionMode mode, size_t initiation_start_index)
{
    CompletionAttemptResult result { mode };

    if (m_next_suggestion_index < m_suggestions.size()) {
        auto& next_suggestion = m_suggestions[m_next_suggestion_index];

        if (mode == CompletePrefix && !next_suggestion.allow_commit_without_listing) {
            result.new_completion_mode = CompletionMode::ShowSuggestions;
            result.avoid_committing_to_single_suggestion = true;
            m_last_shown_suggestion_display_length = 0;
            m_last_shown_suggestion_was_complete = false;
            m_last_shown_suggestion = ByteString::empty();
            return result;
        }

        auto can_complete = next_suggestion.invariant_offset <= m_largest_common_suggestion_prefix_length;
        ssize_t actual_offset;
        size_t shown_length = m_last_shown_suggestion_display_length;
        switch (mode) {
        case CompletePrefix:
            actual_offset = 0;
            break;
        case ShowSuggestions:
            actual_offset = 0 - m_largest_common_suggestion_prefix_length + next_suggestion.invariant_offset;
            if (can_complete && next_suggestion.allow_commit_without_listing)
                shown_length = m_largest_common_suggestion_prefix_length + m_last_shown_suggestion.trivia_view().length();
            break;
        default:
            if (m_last_shown_suggestion_display_length == 0)
                actual_offset = 0;
            else
                actual_offset = 0 - m_last_shown_suggestion_display_length + next_suggestion.invariant_offset;
            break;
        }

        auto& suggestion = suggest();
        set_current_suggestion_initiation_index(initiation_start_index);

        result.offset_region_to_remove = { next_suggestion.invariant_offset, shown_length };
        result.new_cursor_offset = actual_offset;
        result.static_offset_from_cursor = next_suggestion.static_offset;

        if (mode == CompletePrefix) {
            // Only auto-complete *if possible*.
            if (can_complete) {
                result.insert.append(suggestion.text_view().unicode_substring_view(suggestion.invariant_offset, m_largest_common_suggestion_prefix_length - suggestion.invariant_offset));
                m_last_shown_suggestion_display_length = m_largest_common_suggestion_prefix_length;
                // Do not increment the suggestion index, as the first tab should only be a *peek*.
                if (m_suggestions.size() == 1) {
                    // If there's one suggestion, commit and forget.
                    result.new_completion_mode = DontComplete;
                    // Add in the trivia of the last selected suggestion.
                    result.insert.append(suggestion.trailing_trivia.code_points());
                    m_last_shown_suggestion_display_length = 0;
                    result.style_to_apply = suggestion.style;
                    m_last_shown_suggestion_was_complete = true;
                    return result;
                }
            } else {
                m_last_shown_suggestion_display_length = 0;
            }
            result.new_completion_mode = CompletionMode::ShowSuggestions;
            m_last_shown_suggestion_was_complete = false;
            m_last_shown_suggestion = ByteString::empty();
        } else {
            result.insert.append(suggestion.text_view().unicode_substring_view(suggestion.invariant_offset, suggestion.text_view().length() - suggestion.invariant_offset));
            // Add in the trivia of the last selected suggestion.
            result.insert.append(suggestion.trailing_trivia.code_points());
            m_last_shown_suggestion_display_length += suggestion.trivia_view().length();
        }
    } else {
        m_next_suggestion_index = 0;
    }
    return result;
}

ErrorOr<size_t> SuggestionManager::for_each_suggestion(Function<ErrorOr<IterationDecision>(CompletionSuggestion const&, size_t)> callback) const
{
    size_t start_index { 0 };
    for (auto& suggestion : m_suggestions) {
        if (start_index++ < m_last_displayed_suggestion_index)
            continue;
        if (TRY(callback(suggestion, start_index - 1)) == IterationDecision::Break)
            break;
    }
    return start_index;
}

}
