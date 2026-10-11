/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakHashMap.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

enum class SpellingMarkerType : u8 {
    Spelling,
};

// A run of a text node's data the spellchecker flagged, in UTF-16 code units.
struct SpellingMarker {
    size_t start { 0 };
    size_t end { 0 };
    SpellingMarkerType type { SpellingMarkerType::Spelling };

    bool operator==(SpellingMarker const&) const = default;
};

// The misspelling a right-click landed on, which the context menu's guesses replace.
struct ContextMenuMisspelling {
    GC::Weak<DOM::Text> text;
    size_t start { 0 };
    size_t end { 0 };
    Utf16String word;
};

// The spelling markers of a document's text nodes. Script never reaches them:
// "UAs that implement ::spelling-error and ::grammar-error must prevent pages from being able to read the styling of
// such highlighted segments."
// https://drafts.csswg.org/css-pseudo-4/#highlight-security
class SpellingMarkers {
    AK_MAKE_NONCOPYABLE(SpellingMarkers);
    AK_MAKE_NONMOVABLE(SpellingMarkers);
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit SpellingMarkers(DOM::Document&);

    ReadonlySpan<SpellingMarker> markers_for(DOM::Text const&) const;
    Optional<SpellingMarker> marker_containing(DOM::Text const&, size_t offset) const;

    void add(DOM::Text&, size_t start, size_t end);
    void remove_overlapping(DOM::Text&, size_t start, size_t end);
    void remove_all(DOM::Text&);
    void remove_markers_for_word(Utf16View word);

    // Runs from the replace data steps of a text node that holds markers.
    void did_replace_data(DOM::Text&, size_t offset, size_t count, size_t data_length);

    template<typename Callback>
    void for_each_marked_text(Callback callback)
    {
        m_markers.for_each_live_value([&](NodeMarkers& entry) {
            if (auto text = entry.text.ptr(); text && !entry.markers.is_empty())
                callback(*text, entry.markers.span());
        });
    }

    bool is_empty() const { return m_markers.is_empty(); }

    void visit_edges(GC::Cell::Visitor&);

private:
    struct NodeMarkers {
        GC::Weak<DOM::Text> text;
        // Sorted by start; markers never overlap.
        Vector<SpellingMarker> markers;
    };

    void did_change(DOM::Text&);

    GC::Ref<DOM::Document> m_document;
    GC::WeakHashMap<DOM::Text, NodeMarkers> m_markers;
};

}
