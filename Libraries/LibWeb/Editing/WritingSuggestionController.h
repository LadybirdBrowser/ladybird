/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Page/WritingSuggestion.h>

namespace Web::Editing {

// Asks the UI process to predict how the text at the caret continues as the user types, and keeps the prediction that
// shows after the caret. The suggestion is never in the DOM: Layout draws it after the caret until the user accepts it,
// which types it in.
// https://html.spec.whatwg.org/multipage/interaction.html#writing-suggestions
class WEB_API WritingSuggestionController {
    AK_MAKE_NONCOPYABLE(WritingSuggestionController);
    AK_MAKE_NONMOVABLE(WritingSuggestionController);
    AK_ALLOC_WITH_KMALLOC;

public:
    // How much text on each side of the caret goes with a request, in UTF-16 code units.
    static constexpr size_t CONTEXT_SIZE = 512;

    struct ShowingSuggestion {
        GC::Weak<DOM::Text> text;
        // Where the caret is in the text node, which is where the suggestion shows.
        size_t offset { 0 };
        // What the suggestion would add after the caret.
        Utf16String remainder;
    };

    explicit WritingSuggestionController(DOM::Document&);

    static bool is_eligible(DOM::Node const& caret_container);

    Optional<ShowingSuggestion> const& showing() const { return m_showing; }

    void did_focus(GC::Ptr<DOM::Node>);
    // A user edit changed the text of the editable that contains node.
    void did_user_edit(DOM::Node&);
    void did_change_selection();
    // The data of the text node changed, by a user edit or not.
    void did_change_text(DOM::Text const&);
    void did_start_input_method_composition();
    void did_end_input_method_composition();
    void dismiss();

    bool is_settled() const;

    void visit_edges(GC::Cell::Visitor&);

private:
    struct Request {
        u64 generation { 0 };
        GC::Weak<DOM::Text> text;
        size_t offset { 0 };
        // The text sent, and where the caret is in it.
        Utf16String sent_text;
        u32 caret_offset { 0 };
        size_t text_length { 0 };
    };

    void set_showing(Optional<ShowingSuggestion>);
    void schedule_request();
    void run_request();
    void did_receive(Request const&, Optional<WritingSuggestion> const&);
    void schedule_selection_check();
    void run_selection_check();
    bool caret_is_at_showing_suggestion() const;
    static bool can_draw_at(DOM::Text const&);

    GC::Ref<DOM::Document> m_document;
    Optional<ShowingSuggestion> m_showing;
    // The length of the suggestion's text node when the suggestion showed, so typing through can tell an insertion at
    // the caret from any other change.
    size_t m_showing_text_length { 0 };
    // The text right before the caret when the suggestion showed, so text a script changed under it is noticed.
    Utf16String m_showing_prefix;
    // Bumped by every edit, caret move, and dismissal, so an answer to a request sent before one of them is dropped.
    u64 m_generation { 0 };
    size_t m_requests_in_flight { 0 };
    bool m_request_scheduled { false };
    bool m_selection_check_scheduled { false };
};

}
