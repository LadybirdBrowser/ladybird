/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TextTrackCueList.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextTrackCueList);

GC::Ref<TextTrackCueList> TextTrackCueList::create(GC::Ref<TextTrack> track, Subset subset)
{
    return GC::Heap::the().allocate<TextTrackCueList>(track, subset);
}

TextTrackCueList::TextTrackCueList(GC::Ref<TextTrack> track, Subset subset)
    : DOM::EventTarget()
    , m_track(track)
    , m_subset(subset)
{
}

TextTrackCueList::~TextTrackCueList() = default;

void TextTrackCueList::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
}

// FIXME: The cues list should only contain cues whose end times occur at or after the earliest possible position when
//        the script started, and the active cues list should use the active flags as they were when the script started.
template<typename Callback>
void TextTrackCueList::for_each_cue(Callback callback) const
{
    for (auto const& cue : m_track->text_track_list_of_cues()) {
        if (m_subset == Subset::Active && !cue->is_active())
            continue;
        if (callback(cue) == IterationDecision::Break)
            return;
    }
}

GC::Ptr<TextTrackCue> TextTrackCueList::item(size_t index) const
{
    // To determine the value of an indexed property for a given index index, the user agent must return the indexth text track cue in the list
    // represented by the TextTrackCueList object.
    GC::Ptr<TextTrackCue> result;
    for_each_cue([&](auto const& cue) {
        if (index-- == 0) {
            result = cue;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return result;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcuelist-length
size_t TextTrackCueList::length() const
{
    // The length attribute must return the number of cues in the list represented by the TextTrackCueList object.
    size_t length = 0;
    for_each_cue([&](auto const&) {
        ++length;
        return IterationDecision::Continue;
    });
    return length;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcuelist-getcuebyid
GC::Ptr<TextTrackCue> TextTrackCueList::get_cue_by_id(Utf16View id) const
{
    // The getCueById(id) method, when called with an argument other than the empty string, must return the first text track cue in the list
    // represented by the TextTrackCueList object whose text track cue identifier is id, if any, or null otherwise. If the argument is the
    // empty string, then the method must return null.
    if (id.is_empty())
        return nullptr;

    GC::Ptr<TextTrackCue> result;
    for_each_cue([&](auto const& cue) {
        if (cue->id() == id) {
            result = cue;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return result;
}

}
