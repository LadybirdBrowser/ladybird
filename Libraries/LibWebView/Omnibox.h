/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Export.h>
#include <LibWebView/PrivateBrowsing.h>

namespace WebView {

// Feeds autocomplete suggestions to the Omnibox. The default implementation wraps WebView::Autocomplete;
// tests provide a scripted implementation to control delivery content and timing exactly.
class WEBVIEW_API OmniboxSuggestionProvider {
public:
    virtual ~OmniboxSuggestionProvider() = default;

    Function<void(AutocompleteQueryID, Vector<AutocompleteSuggestion>)> on_suggestions;

    virtual void query(AutocompleteQueryID, String, size_t max_suggestions) = 0;
    virtual void cancel() = 0;
    virtual void record_engagement(OmniboxEngagement) { }
};

// The state machine behind a browser location bar editing session.
//
// The chrome forwards user events (keystrokes, popup interactions, focus changes) and renders whatever the
// model instructs through the callbacks below. The model owns the authoritative state: the user's typed
// query, who produced the text currently on display, the suggestion popup contents, and the handshake that
// activates a suggestion once fresh results arrive after Enter. It lives outside any UI toolkit so the
// behavior is unit-testable with a scripted provider and shareable between chromes.
//
// The text on display always has exactly one of three provenances:
//   - UserText:        the text is exactly what the user typed (the query).
//   - InlineCompleted: the query plus a completion suffix borrowed from one specific suggestion. The suffix
//                      is shown selected so the next keystroke replaces it.
//   - RowPreview:      the text of a highlighted suggestion that does not extend the query, shown fully
//                      selected. The query is retained and restored when the popup goes away.
//
// The invariant that makes Enter trustworthy: a selected row is activated only when it belongs to the
// current query generation or the user selected it explicitly. When results lag behind fast typing, Enter
// immediately commits the current input through the browser's normal URL-or-search resolution. It never
// waits for a provider response that may arrive after the user's intent has changed.
class WEBVIEW_API Omnibox {
    AK_MAKE_NONCOPYABLE(Omnibox);
    AK_MAKE_NONMOVABLE(Omnibox);

public:
    explicit Omnibox(IsPrivate);
    explicit Omnibox(NonnullOwnPtr<OmniboxSuggestionProvider>);
    ~Omnibox();

    // What the location bar should display: the full text, and, when a completion or preview is active, the
    // start of the range that should be shown selected (through the end of the text, caret at the end).
    struct Display {
        String text;
        Optional<size_t> selection_start;
    };

    enum class EscapeAction {
        ClosedPopup,
        EndEditing,
    };

    // Events from the chrome:
    void begin_editing(String text);
    void end_editing();
    void set_suspended(bool);
    void show_all_suggestions();
    void text_edited(String text, bool cursor_at_end);
    void cursor_moved(bool cursor_at_end);
    bool accept_completion();
    void will_delete_text();
    void return_pressed();
    EscapeAction escape_pressed();
    void popup_dismissed();
    bool select_next_suggestion();
    bool select_previous_suggestion();
    void suggestion_clicked(size_t suggestion_index);

    // State for the chrome:
    String const& query() const { return m_query; }
    bool is_editing() const { return m_is_editing; }
    bool is_popup_visible() const { return m_popup_visible; }
    Vector<AutocompleteSuggestion> const& suggestions() const { return m_suggestions; }

    Optional<size_t> selected_suggestion() const
    {
        return m_selection.map([](auto const& selection) { return selection.index; });
    }

    // Callbacks to the chrome:
    Function<void(Display const&)> on_display_change;
    Function<void()> on_suggestions_change;
    Function<void()> on_selection_change;
    Function<void(String)> on_commit;

private:
    enum class TextProvenance {
        UserText,
        InlineCompleted,
        RowPreview,
    };

    // The selection carries its own origin, so an explicit-choice verdict cannot outlive or predate the
    // row it is about.
    struct Selection {
        enum class Origin {
            Automatic,
            ExplicitChoice,
        };

        size_t index { 0 };
        Origin origin { Origin::Automatic };
    };

    // Suppression armed by a deleting key press, pinned to the query that the deletion produces, and
    // lifted by any query change. This is what keeps a deleted completion from instantly growing back.
    struct SuppressionArmedByDelete { };
    using CompletionSuppression = Variant<Empty, SuppressionArmedByDelete, String>;

    enum class StepDirection {
        Next,
        Previous,
    };

    void start_query();
    void received_suggestions(AutocompleteQueryID, Vector<AutocompleteSuggestion>);
    void stabilize_automatic_default(Vector<AutocompleteSuggestion>&, bool is_same_generation_refresh) const;
    Optional<size_t> update_completion_for_suggestions(Vector<AutocompleteSuggestion> const&);
    bool select_adjacent_suggestion(StepDirection);
    void highlight_suggestion(size_t suggestion_index, Selection::Origin);
    void display_suggestion(size_t suggestion_index);
    void activate_selected_suggestion();
    void commit_suggestion(size_t suggestion_index, bool record_engagement, bool was_explicit);
    void commit_suggestion_text(String);
    void commit_verbatim(String);
    void adopt_display_text_as_query();
    void apply_completion(String suggestion_text, String completion_text);
    void apply_preview(String suggestion_text);
    void restore_query_display();
    void set_display(String text, Optional<size_t> selection_start);
    void show_popup();
    void close_popup();
    void abandon_popup_session();
    void set_selection(Optional<Selection>);
    void commit(String text);
    bool selection_is_explicit() const;
    bool completion_is_suppressed() const;

    NonnullOwnPtr<OmniboxSuggestionProvider> m_provider;

    bool m_is_editing { false };

    // The chrome temporarily handed input elsewhere (for example a context menu) without ending the
    // session; suggestion deliveries are dropped and completions left alone until it resumes.
    bool m_is_suspended { false };

    TextProvenance m_provenance { TextProvenance::UserText };

    // The user's typed text, surviving completions and previews. Under UserText provenance the display
    // equals the query; the other provenances layer borrowed text over it and restore it on the way out.
    String m_query;
    String m_display_text;

    // Completions only ever append at the end, so they are skipped while the caret is elsewhere; a late
    // delivery must not rewrite the text under a caret the user has moved.
    bool m_cursor_at_end { true };

    // InlineCompleted: the suggestion whose text supplied the completion suffix.
    Optional<String> m_completion_suggestion;

    // Accepting a completion makes the full display text the next query, but committing it without
    // another edit must still teach the prefix that produced the completion.
    Optional<String> m_retained_engagement_input;

    // The user deleted or typed over a completion, so Enter must submit the query as typed instead of
    // activating the popup's automatically selected row. Cleared as soon as a fresh completion is applied.
    bool m_user_rejected_completion { false };

    CompletionSuppression m_completion_suppression { Empty {} };

    // The query ID makes provider delivery and popup freshness independent of comparing user-facing
    // strings. The rows survive close_popup() so the arrow keys can bring a dismissed popup back.
    bool m_popup_visible { false };
    Optional<AutocompleteQueryID> m_popup_query_id;
    Vector<AutocompleteSuggestion> m_suggestions;
    Optional<Selection> m_selection;

    u64 m_next_query_id { 0 };
    Optional<AutocompleteQueryID> m_active_query_id;
};

}
