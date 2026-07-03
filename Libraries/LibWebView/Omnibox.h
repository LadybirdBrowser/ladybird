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

namespace WebView {

// Feeds autocomplete suggestions to the Omnibox. The default implementation wraps WebView::Autocomplete;
// tests provide a scripted implementation to control delivery content and timing exactly.
class WEBVIEW_API OmniboxSuggestionProvider {
public:
    virtual ~OmniboxSuggestionProvider() = default;

    Function<void(Vector<AutocompleteSuggestion>, AutocompleteResultKind)> on_suggestions;

    virtual void query(String, size_t max_suggestions) = 0;
    virtual void cancel() = 0;
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
// The invariant that makes Enter trustworthy: whatever state we are in, on_commit carries exactly what the
// bar displays. Enter resolves in this order: user-edited text is committed verbatim; otherwise, if the
// visible popup rows belong to the current query, the selected row is activated; otherwise the rows are
// stale (the user typed faster than suggestions arrive), so the query is re-run and the activation happens
// as soon as a result worth acting on comes back. The predecessor of this model derived all of that from
// widget selection offsets and a pile of latched booleans, and most omnibox bugs were one of those going
// stale relative to what was actually on screen.
class WEBVIEW_API Omnibox {
    AK_MAKE_NONCOPYABLE(Omnibox);
    AK_MAKE_NONMOVABLE(Omnibox);

public:
    Omnibox();
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
    void will_delete_text();
    void return_pressed();
    EscapeAction escape_pressed();
    void popup_dismissed();
    bool select_next_suggestion();
    bool select_previous_suggestion();
    void suggestion_hovered(size_t suggestion_index);
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

    // The selection carries its own origin, so a "user choice" verdict cannot outlive or predate the row
    // it is about. A deliberate choice wins over m_user_rejected_completion on Enter and survives
    // Escape's completion-preserving path.
    struct Selection {
        enum class Origin {
            Automatic,
            UserChoice,
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

    void received_suggestions(Vector<AutocompleteSuggestion>, AutocompleteResultKind);
    Optional<size_t> update_completion_for_suggestions(Vector<AutocompleteSuggestion> const&);
    bool select_adjacent_suggestion(StepDirection);
    void highlight_suggestion(size_t suggestion_index);
    void activate_selected_suggestion();
    void commit_suggestion_text(String);
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
    bool selection_is_user_choice() const;
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

    // The user deleted or typed over a completion, so Enter must submit the query as typed instead of
    // activating the popup's automatically selected row. Cleared as soon as a fresh completion is applied.
    bool m_user_rejected_completion { false };

    CompletionSuppression m_completion_suppression { Empty {} };

    // m_popup_query records which query produced the rows on display. Rows can lag the query, since
    // intermediate results do not repaint a visible popup and remote results arrive late; comparing it
    // against m_query is how Enter distinguishes "activate what the user sees" from "wait for results".
    // The rows survive close_popup() so the arrow keys can bring a dismissed popup back.
    bool m_popup_visible { false };
    String m_popup_query;
    Vector<AutocompleteSuggestion> m_suggestions;
    Optional<Selection> m_selection;

    // Enter was pressed while the popup rows were stale; activate as soon as fresh results arrive.
    Optional<String> m_pending_activation_query;
};

}
