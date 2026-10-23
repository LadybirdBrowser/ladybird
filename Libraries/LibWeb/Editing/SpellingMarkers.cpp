/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/SpellingMarkers.h>

namespace Web::Editing {

SpellingMarkers::SpellingMarkers(DOM::Document& document)
    : m_document(document)
{
}

void SpellingMarkers::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
}

ReadonlySpan<SpellingMarker> SpellingMarkers::markers_for(DOM::Text const& text) const
{
    if (!text.has_spelling_markers())
        return {};
    auto entry = m_markers.get(text);
    if (!entry.has_value())
        return {};
    return entry->markers.span();
}

Optional<SpellingMarker> SpellingMarkers::marker_containing(DOM::Text const& text, size_t offset) const
{
    for (auto const& marker : markers_for(text)) {
        if (marker.start <= offset && offset <= marker.end)
            return marker;
    }
    return {};
}

void SpellingMarkers::add(DOM::Text& text, size_t start, size_t end)
{
    VERIFY(&text.document() == m_document.ptr());
    VERIFY(start < end && end <= text.length_in_utf16_code_units());
    auto& entry = m_markers.ensure(text, [&] { return NodeMarkers { text, {} }; });
    size_t index = 0;
    while (index < entry.markers.size() && entry.markers[index].start < start)
        ++index;
    entry.markers.insert(index, { start, end, SpellingMarkerType::Spelling });
    did_change(text);
}

void SpellingMarkers::remove_overlapping(DOM::Text& text, size_t start, size_t end)
{
    auto entry = m_markers.get(text);
    if (!entry.has_value())
        return;
    auto removed = entry->markers.remove_all_matching([&](auto const& marker) {
        return marker.start < end && start < marker.end;
    });
    if (removed)
        did_change(text);
}

void SpellingMarkers::remove_all(DOM::Text& text)
{
    if (m_markers.remove(text))
        did_change(text);
}

void SpellingMarkers::remove_markers_for_word(Utf16View word)
{
    GC::RootVector<GC::Ref<DOM::Text>> changed;
    m_markers.for_each_live_value([&](NodeMarkers& entry) {
        auto text = entry.text.ptr();
        if (!text)
            return;
        auto data = text->data().utf16_view();
        auto removed = entry.markers.remove_all_matching([&](auto const& marker) {
            return data.substring_view(marker.start, marker.end - marker.start) == word;
        });
        if (removed)
            changed.append(*text);
    });
    for (auto& text : changed)
        did_change(text);
}

void SpellingMarkers::did_replace_data(DOM::Text& text, size_t offset, size_t count, size_t data_length)
{
    auto entry = m_markers.get(text);
    if (!entry.has_value())
        return;
    auto removed_end = offset + count;
    Vector<SpellingMarker> markers;
    markers.ensure_capacity(entry->markers.size());
    for (auto marker : entry->markers) {
        // Data inserted right after a marker leaves it in place; the next check judges the word.
        if (marker.end <= offset) {
            markers.unchecked_append(marker);
            continue;
        }
        if (marker.start >= removed_end) {
            marker.start = marker.start + data_length - count;
            marker.end = marker.end + data_length - count;
            markers.unchecked_append(marker);
        }
        // Any other marker overlaps the removed data or has the insertion point inside it, so its word changed.
    }
    entry->markers = move(markers);
    did_change(text);
}

void SpellingMarkers::did_change(DOM::Text& text)
{
    auto entry = m_markers.get(text);
    auto has_markers = entry.has_value() && !entry->markers.is_empty();
    if (entry.has_value() && !has_markers)
        m_markers.remove(text);
    text.set_has_spelling_markers({}, has_markers);
    m_document->note_spelling_markers_changed();
}

}
