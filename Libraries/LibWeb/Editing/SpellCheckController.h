/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Page/SpellingError.h>

namespace Web::Editing {

// Decides what text of a document gets spellchecked and when, asks the UI process to check it, and applies the answers
// to the document's spelling markers.
// https://html.spec.whatwg.org/multipage/interaction.html#spelling-and-grammar-checking
class SpellCheckController {
    AK_MAKE_NONCOPYABLE(SpellCheckController);
    AK_MAKE_NONMOVABLE(SpellCheckController);
    AK_ALLOC_WITH_KMALLOC;

public:
    static constexpr size_t TYPING_WINDOW_SIZE = 1024;
    static constexpr size_t EXISTING_TEXT_CHUNK_SIZE = 16384;

    struct WordRange {
        GC::Weak<DOM::Text> text;
        size_t start { 0 };
        size_t end { 0 };
    };

    explicit SpellCheckController(DOM::Document&);

    void did_focus(GC::Ptr<DOM::Node>);
    // A user edit changed the text of the editable that contains node.
    void did_user_edit(DOM::Node&);
    void did_end_input_method_composition();
    void did_change_selection();
    void did_change_checking_state(DOM::Element&);

    bool is_settled() const;

    void visit_edges(GC::Cell::Visitor&);

private:
    struct ParagraphPiece {
        GC::Ref<DOM::Text> text;
        size_t node_start { 0 };
        size_t node_end { 0 };
        bool checked { false };
    };
    using Paragraph = GC::ConservativeVector<ParagraphPiece>;

    struct RequestPiece {
        GC::Weak<DOM::Text> text;
        // The node's data from node_start to node_end, as it was when the text was sent.
        Utf16String snapshot;
        size_t node_start { 0 };
        size_t node_end { 0 };
        // Where the piece starts in the text sent.
        size_t offset { 0 };
        bool checked { false };
    };

    enum class RequestKind : u8 {
        ExistingText,
        Typing,
        Other,
    };

    struct Request {
        RequestKind kind { RequestKind::Other };
        Utf16String text;
        Vector<RequestPiece> pieces;
        // Where the window ended in the paragraph's text.
        size_t window_end { 0 };
    };

    // The editable whose existing text is being checked, one chunk at a time. Each chunk is built from the text as
    // it is when the chunk goes out, so edits made while the pass runs don't make later chunks stale.
    struct ExistingTextPass {
        // The first text node of each paragraph still to check.
        Vector<GC::Weak<DOM::Text>> paragraphs;
        size_t next_paragraph { 0 };
        // Where the next chunk starts in the current paragraph's text.
        size_t next_offset { 0 };
    };

    bool owns(DOM::Text const&) const;
    void send_window_around(DOM::Text&, size_t offset, RequestKind);
    void schedule_typing_check();
    void run_typing_check();
    void schedule_exempt_word_check();
    void run_exempt_word_check();

    Paragraph paragraph_containing(DOM::Text&, size_t offset) const;
    Vector<GC::Weak<DOM::Text>> paragraphs_of(DOM::Node& editable) const;
    Request make_request(Paragraph const&, size_t window_start, size_t window_end, RequestKind) const;
    void send(Request);
    void send_next_existing_text_chunk();
    void did_receive_answer(Request const&, Vector<SpellingError> const&);
    void apply(Request const&, Vector<SpellingError> const&);
    bool overlaps_exempt_word(DOM::Text const&, size_t start, size_t end) const;

    GC::Ref<DOM::Document> m_document;
    Optional<ExistingTextPass> m_existing_text_pass;
    bool m_existing_text_chunk_in_flight { false };
    size_t m_requests_in_flight { 0 };
    // The word the user is still typing, which no answer flags until the caret leaves it.
    Optional<WordRange> m_exempt_word;
    bool m_typing_check_scheduled { false };
    bool m_typing_check_in_flight { false };
    // A typing check wanted while another was in flight, which goes out once that one's answer is in.
    bool m_typing_check_wanted { false };
    bool m_exempt_word_check_scheduled { false };
};

}
