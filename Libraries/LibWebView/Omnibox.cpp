/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWebView/Omnibox.h>

namespace WebView {

// Wraps the real autocomplete machinery (history store, search engine, remote suggestions).
class AutocompleteSuggestionProvider final : public OmniboxSuggestionProvider {
public:
    AutocompleteSuggestionProvider()
    {
        m_autocomplete.on_autocomplete_query_complete = [this](auto suggestions, auto result_kind) {
            if (on_suggestions)
                on_suggestions(move(suggestions), result_kind);
        };
    }

    virtual void query(String query, size_t max_suggestions) override
    {
        m_autocomplete.query_autocomplete_engine(move(query), max_suggestions);
    }

    virtual void cancel() override
    {
        m_autocomplete.cancel_pending_query();
    }

private:
    Autocomplete m_autocomplete;
};

// "theverge.com/" completes "theverge.com" for all practical purposes, so ignore a trailing slash that
// merely closes out a root URL.
static StringView candidate_by_trimming_root_trailing_slash(StringView candidate)
{
    if (!candidate.ends_with('/'))
        return candidate;

    auto host_and_path = candidate;
    for (auto scheme : { "https://"sv, "http://"sv }) {
        if (host_and_path.starts_with(scheme)) {
            host_and_path = host_and_path.substring_view(scheme.length());
            break;
        }
    }

    auto first_slash = host_and_path.find('/');
    if (!first_slash.has_value() || *first_slash != host_and_path.length() - 1)
        return candidate;

    return candidate.substring_view(0, candidate.length() - 1);
}

// Users type addresses without the scheme and often without "www.", so a suggestion is matched in every
// form the typed text could be a spelling of. Invokes the callback with each form until it returns true.
static bool matches_any_candidate_form(String const& suggestion_text, auto callback)
{
    auto trimmed = candidate_by_trimming_root_trailing_slash(suggestion_text);

    if (callback(trimmed))
        return true;

    if (trimmed.starts_with("www."sv) && callback(trimmed.substring_view(4)))
        return true;

    for (auto scheme : { "https://"sv, "http://"sv }) {
        if (!trimmed.starts_with(scheme))
            continue;
        auto stripped = trimmed.substring_view(scheme.length());
        if (callback(stripped))
            return true;
        if (stripped.starts_with("www."sv) && callback(stripped.substring_view(4)))
            return true;
    }

    return false;
}

// NB: Prefix matching is ASCII case-insensitive only. Completion candidates are serialized URLs, so
//     non-ASCII hosts and paths arrive punycoded or percent-encoded and never need Unicode case folding.
static String inline_completion_for_candidate(String const& query, StringView candidate)
{
    auto query_view = query.bytes_as_string_view();
    if (query_view.is_empty() || candidate.length() <= query_view.length())
        return {};
    if (!candidate.starts_with(query_view, CaseSensitivity::CaseInsensitive))
        return {};

    // Keep the prefix exactly as the user typed it and borrow only the completion suffix.
    StringBuilder builder;
    builder.append(query_view);
    builder.append(candidate.substring_view(query_view.length()));
    return MUST(builder.to_string());
}

static String inline_completion_for_suggestion(String const& query, String const& suggestion_text)
{
    String completion;
    matches_any_candidate_form(suggestion_text, [&](StringView candidate) {
        completion = inline_completion_for_candidate(query, candidate);
        return !completion.is_empty();
    });
    return completion;
}

// NB: Unlike completion candidates, search suggestion texts are raw phrases, so the exact-match check
//     folds Unicode case ("Muller" with a u-umlaut must match "muller" with one).
static bool suggestion_matches_query_exactly(String const& query, String const& suggestion_text)
{
    return matches_any_candidate_form(suggestion_text, [&](StringView candidate) {
        return MUST(String::from_utf8(candidate)).equals_ignoring_case(query);
    });
}

static Optional<size_t> index_of_suggestion(String const& suggestion_text, Vector<AutocompleteSuggestion> const& suggestions)
{
    return suggestions.find_first_index_if([&](auto const& suggestion) {
        return suggestion.text == suggestion_text;
    });
}

Omnibox::Omnibox()
    : Omnibox(make<AutocompleteSuggestionProvider>())
{
}

Omnibox::Omnibox(NonnullOwnPtr<OmniboxSuggestionProvider> provider)
    : m_provider(move(provider))
{
    m_provider->on_suggestions = [this](auto suggestions, auto result_kind) {
        received_suggestions(move(suggestions), result_kind);
    };
}

Omnibox::~Omnibox() = default;

void Omnibox::begin_editing(String text)
{
    m_is_editing = true;
    m_is_suspended = false;
    m_provenance = TextProvenance::UserText;
    m_query = text;
    m_display_text = move(text);
    m_cursor_at_end = true;
    m_completion_suggestion = {};
    m_completion_suppression = Empty {};
    m_user_rejected_completion = false;
    m_pending_activation_query = {};

    // A fresh editing session must not inherit the previous session's popup rows or act on deliveries
    // still in flight for the previous query.
    m_provider->cancel();
    close_popup();
    m_suggestions.clear();
    m_popup_query = {};
}

void Omnibox::end_editing()
{
    if (!m_is_editing)
        return;

    restore_query_display();

    m_is_editing = false;
    m_is_suspended = false;
    m_completion_suppression = Empty {};
    m_user_rejected_completion = false;
    m_pending_activation_query = {};
    m_provider->cancel();
    close_popup();
}

void Omnibox::show_all_suggestions()
{
    if (!m_is_editing)
        return;

    // The bar holds a complete address (or nothing); suggest matches for it without completing over it.
    // An active completion becomes part of that address, so adopt the full display text as the query
    // instead of truncating the bar back to the typed prefix when results arrive.
    adopt_display_text_as_query();
    m_completion_suppression = m_query;
    m_popup_query = {};
    m_provider->query(m_query, default_autocomplete_suggestion_limit);
}

void Omnibox::text_edited(String text, bool cursor_at_end)
{
    if (!m_is_editing)
        return;

    bool had_completion = m_provenance == TextProvenance::InlineCompleted;
    bool had_preview = m_provenance == TextProvenance::RowPreview;
    auto previous_completion_suggestion = move(m_completion_suggestion);
    m_completion_suggestion = {};

    m_provenance = TextProvenance::UserText;
    m_query = text;
    m_display_text = move(text);
    m_cursor_at_end = cursor_at_end;

    if (m_pending_activation_query.has_value() && *m_pending_activation_query != m_query)
        m_pending_activation_query = {};

    if (m_completion_suppression.has<SuppressionArmedByDelete>()) {
        // The user deleted part of the text; leave it alone until the query changes again.
        if (had_completion || had_preview)
            m_user_rejected_completion = true;
        m_completion_suppression = m_query;
    } else if (auto const* suppressed_query = m_completion_suppression.get_pointer<String>(); suppressed_query && *suppressed_query != m_query) {
        m_completion_suppression = Empty {};
    }

    if (had_preview)
        m_user_rejected_completion = true;

    if (!completion_is_suppressed() && !m_is_suspended && previous_completion_suggestion.has_value()) {
        if (suggestion_matches_query_exactly(m_query, *previous_completion_suggestion)) {
            // The user typed out the whole suggestion; there is nothing left to complete.
        } else if (auto completion = inline_completion_for_suggestion(m_query, *previous_completion_suggestion); !completion.is_empty()) {
            // The user typed further into the completion; keep it.
            apply_completion(previous_completion_suggestion.release_value(), move(completion));
        } else {
            // The user typed over the completion; Enter must now submit the text as typed.
            m_user_rejected_completion = true;
        }
    }

    m_provider->query(m_query, default_autocomplete_suggestion_limit);
}

void Omnibox::cursor_moved(bool cursor_at_end)
{
    if (!m_is_editing)
        return;
    m_cursor_at_end = cursor_at_end;
}

void Omnibox::set_suspended(bool suspended)
{
    if (!m_is_editing)
        return;
    m_is_suspended = suspended;
}

void Omnibox::will_delete_text()
{
    if (!m_is_editing)
        return;
    m_completion_suppression = SuppressionArmedByDelete {};
}

bool Omnibox::completion_is_suppressed() const
{
    auto const* suppressed_query = m_completion_suppression.get_pointer<String>();
    return suppressed_query && *suppressed_query == m_query;
}

bool Omnibox::selection_is_user_choice() const
{
    return m_selection.has_value() && m_selection->origin == Selection::Origin::UserChoice;
}

void Omnibox::received_suggestions(Vector<AutocompleteSuggestion> suggestions, AutocompleteResultKind result_kind)
{
    if (!m_is_editing || m_is_suspended)
        return;

    bool should_activate = m_pending_activation_query.has_value() && *m_pending_activation_query == m_query;
    bool had_row_preview = m_provenance == TextProvenance::RowPreview;

    Optional<size_t> selected_suggestion;
    if (had_row_preview)
        selected_suggestion = index_of_suggestion(m_display_text, suggestions);
    else
        selected_suggestion = update_completion_for_suggestions(suggestions);

    // Do not update the popup while results are still changing. Intermediate updates are triggered on
    // every keystroke and would cause visible flicker in the suggestion list. Only final results are used
    // to refresh the UI, except when Enter is already waiting on them.
    if (result_kind == AutocompleteResultKind::Intermediate && m_popup_visible && !should_activate)
        return;

    // A selection that survives the refresh pointing at the same suggestion is still the user's
    // deliberate choice; anything else the refresh picks is automatic.
    Optional<String> user_chosen_text;
    if (selection_is_user_choice() && m_selection->index < m_suggestions.size())
        user_chosen_text = m_suggestions[m_selection->index].text;

    m_popup_query = m_query;
    m_suggestions = move(suggestions);

    // The fresh selection is published to the chrome by the popup repaint below, not via
    // on_selection_change, so it is assigned directly.
    m_selection = selected_suggestion.map([&](auto index) {
        auto is_user_choice = user_chosen_text.has_value() && m_suggestions[index].text == *user_chosen_text;
        return Selection { index, is_user_choice ? Selection::Origin::UserChoice : Selection::Origin::Automatic };
    });

    // A previewed row that vanished from the refreshed rows must stop being displayed; the bar must not
    // show something Enter would no longer act on.
    if (had_row_preview && !m_selection.has_value())
        restore_query_display();

    if (m_suggestions.is_empty()) {
        // NB: The pending activation survives this close; an empty intermediate result must not eat an
        //     activation the final result may still satisfy.
        if (m_popup_visible) {
            m_popup_visible = false;
            m_selection = {};
            if (on_suggestions_change)
                on_suggestions_change();
        }
        restore_query_display();
    } else {
        // Repaint even when the popup is already visible: the rows changed under it.
        m_popup_visible = true;
        if (on_suggestions_change)
            on_suggestions_change();
    }

    if (should_activate) {
        Optional<String> selected_text;
        if (m_selection.has_value())
            selected_text = m_suggestions[m_selection->index].text;

        if (result_kind == AutocompleteResultKind::Final || (selected_text.has_value() && *selected_text != m_query)) {
            m_pending_activation_query = {};
            activate_selected_suggestion();
        }
    }
}

// Decides which row the popup should select for freshly received suggestions, and applies or clears the
// inline completion accordingly. Row 0 drives both the highlight and (if its text prefix-matches the query)
// the completion preview: the user-visible rule is "the top row is the default action".
Optional<size_t> Omnibox::update_completion_for_suggestions(Vector<AutocompleteSuggestion> const& suggestions)
{
    if (!m_cursor_at_end)
        return {};

    if (suggestions.is_empty())
        return {};

    // A literal URL always wins: no completion, restore the typed text.
    if (suggestions.first().source == AutocompleteSuggestionSource::LiteralURL) {
        restore_query_display();
        return 0;
    }

    // Backspace suppression: the user just deleted into this query, so don't re-apply a completion, but
    // still honor the "highlight the top row" rule.
    if (completion_is_suppressed()) {
        restore_query_display();
        return 0;
    }

    // Preserve an existing completion if its suggestion is still present and still extends the typed
    // prefix. This keeps the completion stable while the user is forward-typing into a suggestion.
    if (m_completion_suggestion.has_value()) {
        if (auto preserved = index_of_suggestion(*m_completion_suggestion, suggestions); preserved.has_value()) {
            if (auto completion = inline_completion_for_suggestion(m_query, *m_completion_suggestion); !completion.is_empty()) {
                apply_completion(*m_completion_suggestion, move(completion));
                return preserved;
            }
        }
    }

    if (auto completion = inline_completion_for_suggestion(m_query, suggestions.first().text); !completion.is_empty()) {
        apply_completion(suggestions.first().text, move(completion));
        return 0;
    }

    // Row 0 does not prefix-match the query: clear any stale completion, restore the typed text, and
    // still highlight row 0.
    restore_query_display();
    return 0;
}

void Omnibox::return_pressed()
{
    if (!m_is_editing || m_display_text.is_empty())
        return;

    if (!m_popup_visible) {
        commit(m_display_text);
        return;
    }

    // The user edited or deleted a completion, so submit the text as typed instead of the popup's
    // automatically selected row. Rows the user chose deliberately still win.
    if (m_user_rejected_completion && !selection_is_user_choice()) {
        Optional<String> selected_text;
        if (m_popup_query == m_query && m_selection.has_value())
            selected_text = m_suggestions[m_selection->index].text;
        if (!completion_is_suppressed() && selected_text.has_value() && *selected_text != m_query) {
            activate_selected_suggestion();
            return;
        }

        auto query = m_query;
        restore_query_display();
        close_popup();
        commit(move(query));
        return;
    }

    if (m_popup_query == m_query) {
        activate_selected_suggestion();
        return;
    }

    // The visible rows belong to an older query; re-run the current one and activate once results arrive.
    m_pending_activation_query = m_query;
    m_provider->query(m_query, default_autocomplete_suggestion_limit);
}

// The chrome dismissed the popup without a key press (for example a click elsewhere in the window).
void Omnibox::popup_dismissed()
{
    if (!m_is_editing || !m_popup_visible)
        return;

    restore_query_display();
    abandon_popup_session();
}

Omnibox::EscapeAction Omnibox::escape_pressed()
{
    if (!m_is_editing || !m_popup_visible)
        return EscapeAction::EndEditing;

    if (m_provenance == TextProvenance::InlineCompleted && !selection_is_user_choice()) {
        // Keep an automatic completion on display; only the popup goes away.
        adopt_display_text_as_query();
    } else {
        restore_query_display();
    }

    abandon_popup_session();
    return EscapeAction::ClosedPopup;
}

// The user walked away from the popup (Escape or a click elsewhere): Enter must neither submit stale
// edited text nor fire a previously pending activation.
void Omnibox::abandon_popup_session()
{
    m_user_rejected_completion = false;
    m_pending_activation_query = {};
    close_popup();
    m_provider->cancel();
}

bool Omnibox::select_next_suggestion()
{
    return select_adjacent_suggestion(StepDirection::Next);
}

bool Omnibox::select_previous_suggestion()
{
    return select_adjacent_suggestion(StepDirection::Previous);
}

bool Omnibox::select_adjacent_suggestion(StepDirection direction)
{
    if (!m_is_editing || m_suggestions.is_empty())
        return false;

    auto count = m_suggestions.size();
    auto step = direction == StepDirection::Next ? 1 : count - 1;
    auto index = direction == StepDirection::Next ? 0 : count - 1;
    if (!m_popup_visible)
        show_popup();
    else if (m_selection.has_value())
        index = (m_selection->index + step) % count;

    highlight_suggestion(index);
    return true;
}

void Omnibox::suggestion_hovered(size_t suggestion_index)
{
    if (!m_is_editing || suggestion_index >= m_suggestions.size())
        return;
    if (m_selection.has_value() && m_selection->index == suggestion_index)
        return;

    highlight_suggestion(suggestion_index);
}

void Omnibox::suggestion_clicked(size_t suggestion_index)
{
    if (!m_is_editing || suggestion_index >= m_suggestions.size())
        return;

    commit_suggestion_text(m_suggestions[suggestion_index].text);
}

// Highlighting is always a deliberate act (arrow keys or hover); automatic row selection goes through
// received_suggestions() instead.
void Omnibox::highlight_suggestion(size_t suggestion_index)
{
    set_selection(Selection { suggestion_index, Selection::Origin::UserChoice });

    auto const& suggestion_text = m_suggestions[suggestion_index].text;

    if (suggestion_matches_query_exactly(m_query, suggestion_text)) {
        restore_query_display();
        return;
    }

    if (auto completion = inline_completion_for_suggestion(m_query, suggestion_text); !completion.is_empty()) {
        apply_completion(suggestion_text, move(completion));
        return;
    }

    apply_preview(suggestion_text);
}

void Omnibox::activate_selected_suggestion()
{
    if (m_selection.has_value()) {
        commit_suggestion_text(m_suggestions[m_selection->index].text);
        return;
    }

    restore_query_display();
    close_popup();
    commit(m_query);
}

void Omnibox::commit_suggestion_text(String text)
{
    m_provenance = TextProvenance::UserText;
    m_completion_suggestion = {};
    m_query = text;
    set_display(text, {});
    close_popup();
    commit(move(text));
}

void Omnibox::adopt_display_text_as_query()
{
    m_provenance = TextProvenance::UserText;
    m_completion_suggestion = {};
    m_query = m_display_text;
}

void Omnibox::apply_completion(String suggestion_text, String completion_text)
{
    auto completion_start = m_query.bytes_as_string_view().length();

    m_provenance = TextProvenance::InlineCompleted;
    m_completion_suggestion = move(suggestion_text);

    // A fresh automatic completion is visible from here on, so Enter must activate it instead of
    // submitting previously edited text.
    m_user_rejected_completion = false;

    if (m_display_text == completion_text)
        return;
    set_display(move(completion_text), completion_start);
}

void Omnibox::apply_preview(String suggestion_text)
{
    m_provenance = TextProvenance::RowPreview;
    m_completion_suggestion = {};

    if (m_display_text == suggestion_text)
        return;
    set_display(move(suggestion_text), 0);
}

// Drops any completion or preview and puts the typed query back on display. A no-op when the user's own
// text is already showing, so callers need not check the provenance first.
void Omnibox::restore_query_display()
{
    m_provenance = TextProvenance::UserText;
    m_completion_suggestion = {};

    if (m_display_text == m_query)
        return;
    set_display(m_query, {});
}

void Omnibox::set_display(String text, Optional<size_t> selection_start)
{
    m_display_text = move(text);
    if (on_display_change)
        on_display_change({ .text = m_display_text, .selection_start = selection_start });
}

void Omnibox::show_popup()
{
    if (m_popup_visible)
        return;

    m_popup_visible = true;
    if (on_suggestions_change)
        on_suggestions_change();
}

// Hides the popup; the rows are kept so the arrow keys can bring it back.
void Omnibox::close_popup()
{
    if (!m_popup_visible)
        return;

    m_popup_visible = false;
    m_selection = {};
    if (on_suggestions_change)
        on_suggestions_change();
}

void Omnibox::set_selection(Optional<Selection> selection)
{
    bool index_changed = selection.map([](auto const& s) { return s.index; }) != selected_suggestion();
    m_selection = move(selection);
    if (index_changed && on_selection_change)
        on_selection_change();
}

void Omnibox::commit(String text)
{
    if (on_commit)
        on_commit(move(text));
}

}
