/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWebView/AutocompleteMuxer.h>
#include <LibWebView/Omnibox.h>
#include <LibWebView/URL.h>

namespace WebView {

// Wraps the real autocomplete machinery (history store, search engine, remote suggestions).
class AutocompleteSuggestionProvider final : public OmniboxSuggestionProvider {
public:
    AutocompleteSuggestionProvider(IsPrivate is_private)
        : m_autocomplete(is_private)
    {
        m_autocomplete.on_autocomplete_query_complete = [this](auto query_id, auto suggestions, auto) {
            if (on_suggestions)
                on_suggestions(query_id, move(suggestions));
        };
    }

    virtual void query(AutocompleteQueryID query_id, String query, size_t max_suggestions) override
    {
        m_autocomplete.query_autocomplete_engine(query_id, move(query), max_suggestions);
    }

    virtual void cancel() override
    {
        m_autocomplete.cancel_pending_query();
    }

    virtual void record_engagement(OmniboxEngagement engagement) override
    {
        m_autocomplete.record_engagement(move(engagement));
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

static Optional<size_t> index_of_suggestion(AutocompleteSuggestion const& needle, Vector<AutocompleteSuggestion> const& suggestions)
{
    return suggestions.find_first_index_if([&](auto const& suggestion) {
        return autocomplete_suggestions_have_same_destination(needle, suggestion);
    });
}

static Optional<size_t> index_of_automatic_default(Vector<AutocompleteSuggestion> const& suggestions)
{
    return suggestions.find_first_index_if([](auto const& suggestion) {
        return suggestion.can_be_automatically_selected;
    });
}

static bool challenger_is_materially_better(AutocompleteSuggestion const& challenger, AutocompleteSuggestion const& incumbent)
{
    auto challenger_relevance = static_cast<i64>(challenger.relevance);
    auto incumbent_relevance = static_cast<i64>(incumbent.relevance);
    return challenger_relevance - incumbent_relevance > 100
        && challenger_relevance * 10 > incumbent_relevance * 11;
}

static void move_suggestion_to_front(Vector<AutocompleteSuggestion>& suggestions, size_t index)
{
    if (index == 0)
        return;
    auto suggestion = suggestions.take(index);
    suggestions.insert(0, move(suggestion));
}

static Optional<size_t> index_of_exact_search_suggestion(String const& query, Vector<AutocompleteSuggestion> const& suggestions)
{
    return suggestions.find_first_index_if([&](auto const& suggestion) {
        return suggestion.source == AutocompleteSuggestionSource::Search
            && suggestion_matches_query_exactly(query, suggestion.text);
    });
}

Omnibox::Omnibox(IsPrivate is_private)
    : Omnibox(make<AutocompleteSuggestionProvider>(is_private))
{
}

Omnibox::Omnibox(NonnullOwnPtr<OmniboxSuggestionProvider> provider)
    : m_provider(move(provider))
{
    m_provider->on_suggestions = [this](auto query_id, auto suggestions) {
        received_suggestions(query_id, move(suggestions));
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
    m_retained_engagement_input = {};
    m_completion_suppression = Empty {};
    m_user_rejected_completion = false;
    m_active_query_id = {};

    // A fresh editing session must not inherit the previous session's popup rows or act on deliveries
    // still in flight for the previous query.
    m_provider->cancel();
    close_popup();
    m_suggestions.clear();
    m_popup_query_id = {};
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
    m_retained_engagement_input = {};
    m_active_query_id = {};
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
    m_popup_query_id = {};
    start_query();
}

void Omnibox::text_edited(String text, bool cursor_at_end)
{
    if (!m_is_editing)
        return;

    bool had_completion = m_provenance == TextProvenance::InlineCompleted;
    bool had_preview = m_provenance == TextProvenance::RowPreview;
    auto previous_completion_suggestion = move(m_completion_suggestion);
    m_completion_suggestion = {};
    m_retained_engagement_input = {};

    m_provenance = TextProvenance::UserText;
    m_query = text;
    m_display_text = move(text);
    m_cursor_at_end = cursor_at_end;

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

    start_query();
}

void Omnibox::start_query()
{
    ++m_next_query_id;
    VERIFY(m_next_query_id != 0);
    m_active_query_id = m_next_query_id;
    m_provider->query(*m_active_query_id, m_query, default_autocomplete_suggestion_limit);
}

void Omnibox::cursor_moved(bool cursor_at_end)
{
    if (!m_is_editing)
        return;
    m_cursor_at_end = cursor_at_end;

    if (cursor_at_end || m_provenance == TextProvenance::UserText)
        return;

    m_completion_suppression = m_query;
    m_user_rejected_completion = true;
    set_selection({});
    restore_query_display();
}

bool Omnibox::accept_completion()
{
    if (!m_is_editing || m_provenance != TextProvenance::InlineCompleted)
        return false;

    m_retained_engagement_input = m_query;
    adopt_display_text_as_query();
    m_completion_suppression = m_query;
    m_user_rejected_completion = false;
    m_cursor_at_end = true;
    set_display(m_query, {});
    start_query();
    return true;
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

bool Omnibox::selection_is_explicit() const
{
    return m_selection.has_value() && m_selection->origin == Selection::Origin::ExplicitChoice;
}

void Omnibox::received_suggestions(AutocompleteQueryID query_id, Vector<AutocompleteSuggestion> suggestions)
{
    if (!m_is_editing || m_is_suspended || m_active_query_id != query_id)
        return;

    auto is_same_generation_refresh = m_popup_query_id == query_id;
    AutocompleteSuggestion const* previous_explicit_selection = nullptr;
    Optional<Selection::Origin> previous_selection_origin;
    if (is_same_generation_refresh && m_selection.has_value() && m_selection->origin != Selection::Origin::Automatic && m_selection->index < m_suggestions.size()) {
        previous_explicit_selection = &m_suggestions[m_selection->index];
        previous_selection_origin = m_selection->origin;
    }

    stabilize_automatic_default(suggestions, is_same_generation_refresh);

    Optional<size_t> preserved_selection;
    if (previous_explicit_selection)
        preserved_selection = index_of_suggestion(*previous_explicit_selection, suggestions);

    m_popup_query_id = query_id;
    m_suggestions = move(suggestions);

    if (preserved_selection.has_value()) {
        // The fresh selection is published to the chrome by the popup repaint below, not via
        // on_selection_change, so it is assigned directly.
        m_selection = Selection { *preserved_selection, *previous_selection_origin };
        display_suggestion(*preserved_selection);
    } else if (previous_explicit_selection) {
        // A previewed or explicitly selected row that vanished must stop being displayed. Enter must
        // not silently fall through to a different automatic result.
        m_selection = {};
        restore_query_display();
    } else {
        m_selection = update_completion_for_suggestions(m_suggestions).map([](auto index) {
            return Selection { index, Selection::Origin::Automatic };
        });
    }

    if (m_suggestions.is_empty()) {
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
}

void Omnibox::stabilize_automatic_default(Vector<AutocompleteSuggestion>& suggestions, bool is_same_generation_refresh) const
{
    auto challenger_index = index_of_automatic_default(suggestions);
    if (!challenger_index.has_value())
        return;

    // Forward typing may carry a completion into a new generation. Keep it only while it remains a
    // valid default and inline candidate, and is within the inline hysteresis margin of the best
    // inline candidate. A materially better non-inline default clears the completion instead.
    auto has_explicit_selection = is_same_generation_refresh
        && m_selection.has_value()
        && m_selection->origin != Selection::Origin::Automatic;
    if (!has_explicit_selection && m_completion_suggestion.has_value()) {
        auto completion_index = index_of_suggestion(*m_completion_suggestion, suggestions);
        if (completion_index.has_value()) {
            auto const& completion_candidate = suggestions[*completion_index];
            auto const& challenger = suggestions[*challenger_index];
            if (completion_candidate.can_be_automatically_selected
                && completion_candidate.can_be_inline_completed
                && challenger.can_be_inline_completed
                && !inline_completion_for_suggestion(m_query, completion_candidate.text).is_empty()) {
                auto best_inline_relevance = completion_candidate.relevance;
                for (auto const& suggestion : suggestions) {
                    if (suggestion.can_be_inline_completed)
                        best_inline_relevance = max(best_inline_relevance, suggestion.relevance);
                }
                if (static_cast<i64>(completion_candidate.relevance) + 150 >= best_inline_relevance) {
                    move_suggestion_to_front(suggestions, *completion_index);
                    return;
                }
            }
        }
    }

    if (!is_same_generation_refresh)
        return;

    auto previous_default_index = index_of_automatic_default(m_suggestions);
    if (!previous_default_index.has_value())
        return;
    auto incumbent_index = index_of_suggestion(m_suggestions[*previous_default_index], suggestions);
    if (!incumbent_index.has_value() || !suggestions[*incumbent_index].can_be_automatically_selected)
        return;

    if (!challenger_is_materially_better(suggestions[*challenger_index], suggestions[*incumbent_index]))
        move_suggestion_to_front(suggestions, *incumbent_index);
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

    if (!suggestions.first().can_be_automatically_selected) {
        restore_query_display();
        return index_of_exact_search_suggestion(m_query, suggestions);
    }

    // A literal URL always wins: no completion, restore the typed text.
    if (suggestions.first().source == AutocompleteSuggestionSource::LiteralURL) {
        restore_query_display();
        return 0;
    }

    if (!suggestions.first().can_be_inline_completed) {
        restore_query_display();
        return 0;
    }

    // Backspace suppression: the user just deleted into this query, so don't re-apply a completion, but
    // still honor the "highlight the top row" rule.
    if (completion_is_suppressed()) {
        restore_query_display();
        return 0;
    }

    // stabilize_automatic_default() moves a preserved completion to row 0 so the displayed suffix and
    // Enter action can never refer to different candidates.
    if (m_completion_suggestion.has_value() && suggestions.first().text == *m_completion_suggestion) {
        if (auto completion = inline_completion_for_suggestion(m_query, *m_completion_suggestion); !completion.is_empty()) {
            apply_completion(*m_completion_suggestion, move(completion));
            return 0;
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
        commit_verbatim(m_query);
        return;
    }

    if (selection_is_explicit()) {
        activate_selected_suggestion();
        return;
    }

    // The user edited or deleted a completion, so submit the text as typed instead of the popup's
    // automatically selected row. Rows the user chose deliberately still win.
    if (m_user_rejected_completion && !selection_is_explicit()) {
        Optional<String> selected_text;
        if (m_popup_query_id == m_active_query_id && m_selection.has_value())
            selected_text = m_suggestions[m_selection->index].text;
        if (!completion_is_suppressed() && selected_text.has_value() && *selected_text != m_query) {
            activate_selected_suggestion();
            return;
        }

        auto query = m_query;
        restore_query_display();
        close_popup();
        commit_verbatim(move(query));
        return;
    }

    if (m_popup_query_id == m_active_query_id) {
        activate_selected_suggestion();
        return;
    }

    // Results must never delay Enter or activate after the user has moved on. If the visible rows are
    // stale, commit the exact current input through the browser's normal URL-or-search resolution.
    auto query = m_query;
    restore_query_display();
    close_popup();
    commit_verbatim(move(query));
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

    if (m_provenance == TextProvenance::InlineCompleted && !selection_is_explicit())
        m_completion_suppression = m_query;
    restore_query_display();

    abandon_popup_session();
    return EscapeAction::ClosedPopup;
}

// The user walked away from the popup (Escape or a click elsewhere). Invalidate the active generation
// before canceling its provider work so buffered deliveries cannot reopen the popup.
void Omnibox::abandon_popup_session()
{
    m_user_rejected_completion = false;
    m_active_query_id = {};
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

    highlight_suggestion(index, Selection::Origin::ExplicitChoice);
    return true;
}

void Omnibox::suggestion_clicked(size_t suggestion_index)
{
    if (!m_is_editing || suggestion_index >= m_suggestions.size())
        return;

    commit_suggestion(suggestion_index, true, true);
}

void Omnibox::highlight_suggestion(size_t suggestion_index, Selection::Origin origin)
{
    set_selection(Selection { suggestion_index, origin });

    display_suggestion(suggestion_index);
}

void Omnibox::display_suggestion(size_t suggestion_index)
{
    VERIFY(suggestion_index < m_suggestions.size());

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
        auto was_explicit = m_selection->origin == Selection::Origin::ExplicitChoice;
        commit_suggestion(m_selection->index, true, was_explicit);
        return;
    }

    restore_query_display();
    close_popup();
    commit_verbatim(m_query);
}

void Omnibox::commit_suggestion(size_t suggestion_index, bool should_record_engagement, bool was_explicit)
{
    auto suggestion = m_suggestions[suggestion_index];
    if (should_record_engagement) {
        m_provider->record_engagement({
            .input = m_retained_engagement_input.value_or(m_query),
            .destination_kind = suggestion.source == AutocompleteSuggestionSource::Search
                ? OmniboxDestinationKind::Search
                : OmniboxDestinationKind::URL,
            .destination = suggestion.text,
            .was_explicit = was_explicit,
        });
    }
    commit_suggestion_text(move(suggestion.text));
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

void Omnibox::commit_verbatim(String text)
{
    m_provider->record_engagement({
        .input = m_retained_engagement_input.value_or(text),
        .destination_kind = location_looks_like_url(text) ? OmniboxDestinationKind::URL : OmniboxDestinationKind::Search,
        .destination = text,
        .was_explicit = false,
    });
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
