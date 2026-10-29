/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/QuickSort.h>
#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLTrackElement.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TextTrackCue.h>
#include <LibWeb/HTML/TextTrackCueList.h>
#include <LibWeb/HTML/TextTrackObserver.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextTrack);

GC::Ref<TextTrack> TextTrack::create()
{
    return GC::Heap::the().allocate<TextTrack>();
}

TextTrack::TextTrack()
    : DOM::EventTarget()
{
}

TextTrack::~TextTrack() = default;

void TextTrack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_observers);
    visitor.visit(m_text_track_list_of_cues);
    visitor.visit(m_track_element);
    visitor.visit(m_media_element);
    visitor.visit(m_cues);
    visitor.visit(m_active_cues);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-cues
GC::Ptr<TextTrackCueList> TextTrack::cues()
{
    // 1. If this's mode is the text track disabled mode, then return null.
    if (m_mode == TextTrackMode::Disabled)
        return nullptr;

    // 2. Return a live TextTrackCueList object that represents the subset of this's text track list of cues whose end
    //    times occur at or after the earliest possible position when the script started, in text track cue order.
    // For each TextTrack object, when an object is returned, the same TextTrackCueList object must be returned each time.
    if (!m_cues)
        m_cues = TextTrackCueList::create(*this, TextTrackCueList::Subset::All);
    return m_cues;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-activecues
GC::Ptr<TextTrackCueList> TextTrack::active_cues()
{
    // 1. If this's mode is the text track disabled mode, then return null.
    if (m_mode == TextTrackMode::Disabled)
        return nullptr;

    // 2. Return a live TextTrackCueList object that represents the subset of this's text track list of cues whose active
    //    flag was set when the script started, in text track cue order.
    // For each TextTrack object, when an object is returned, the same TextTrackCueList object must be returned each time.
    if (!m_active_cues)
        m_active_cues = TextTrackCueList::create(*this, TextTrackCueList::Subset::Active);
    return m_active_cues;
}

// https://html.spec.whatwg.org/multipage/media.html#text-track-cue-order
static bool cue_order_precedes(TextTrackCue const& a, TextTrackCue const& b)
{
    // Within each group, cues must be sorted by their start time, earliest first; then, any cues with the same start
    // time must be sorted by their end time, latest first; and finally, any cues with identical end times must be
    // sorted in the order they were last added to their respective text track list of cues, oldest first.
    if (a.start_time() != b.start_time())
        return a.start_time() < b.start_time();
    if (a.end_time() != b.end_time())
        return a.end_time() > b.end_time();
    return a.order_added() < b.order_added();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-addcue
void TextTrack::add_cue(GC::Ref<TextTrackCue> cue)
{
    // 1. Let list be this's text track list of cues.
    // FIXME: 2. If list does not yet have any associated rules for updating the text track rendering, then associate list
    //           with the rules for updating the text track rendering appropriate to cue.
    // FIXME: 3. If list's associated rules for updating the text track rendering are not the same rules for updating the
    //           text track rendering as appropriate for cue, then throw an "InvalidStateError" DOMException.

    // 4. If the given cue is in a text track list of cues, then remove cue from that text track list of cues.
    if (auto track = cue->track()) {
        track->m_text_track_list_of_cues.remove_first_matching([&](auto const& entry) { return entry == cue; });
        cue->set_active(false);
        if (track->m_media_element)
            track->m_media_element->text_track_cue_or_text_track_removed();
    }

    // 5. Add cue to list.
    cue->set_track(this);
    cue->set_order_added(m_next_cue_order_added++);
    auto index = lower_bound_index(m_text_track_list_of_cues, cue, [](auto const& entry, auto const& new_cue) {
        return cue_order_precedes(*entry, *new_cue) ? -1 : 1;
    });
    m_text_track_list_of_cues.insert(index, cue);

    if (m_media_element)
        m_media_element->add_newly_introduced_cues({ &cue, 1 });
}

// https://html.spec.whatwg.org/multipage/media.html#text-track-cue-order
void TextTrack::sort_text_track_list_of_cues()
{
    quick_sort(m_text_track_list_of_cues, [](auto const& a, auto const& b) {
        return cue_order_precedes(*a, *b);
    });
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-removecue
WebIDL::ExceptionOr<void> TextTrack::remove_cue(GC::Ref<TextTrackCue> cue)
{
    // 1. If the given cue is not in this's text track list of cues, then throw a "NotFoundError" DOMException.
    auto index = m_text_track_list_of_cues.find_first_index(cue);
    if (!index.has_value())
        return WebIDL::NotFoundError::create("Cue is not in this text track's list of cues"_utf16);

    // 2. Remove cue from this's text track list of cues.
    m_text_track_list_of_cues.remove(*index);
    cue->set_track(nullptr);
    cue->set_active(false);

    if (m_media_element)
        m_media_element->text_track_cue_or_text_track_removed();
    return {};
}

void TextTrack::empty_text_track_list_of_cues()
{
    if (m_text_track_list_of_cues.is_empty())
        return;

    for (auto const& cue : m_text_track_list_of_cues) {
        cue->set_track(nullptr);
        cue->set_active(false);
    }
    m_text_track_list_of_cues.clear();

    if (m_media_element)
        m_media_element->text_track_cue_or_text_track_removed();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-kind
TextTrackKind TextTrack::kind()
{
    return m_kind;
}

void TextTrack::set_kind(TextTrackKind kind)
{
    m_kind = kind;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-label
Utf16String const& TextTrack::label()
{
    return m_label;
}

void TextTrack::set_label(Utf16View label)
{
    m_label = Utf16String::from_utf16(label);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-language
Utf16String const& TextTrack::language()
{
    return m_language;
}

void TextTrack::set_language(Utf16View language)
{
    m_language = Utf16String::from_utf16(language);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-id
Utf16String const& TextTrack::id()
{
    return m_id;
}

void TextTrack::set_id(Utf16View id)
{
    m_id = Utf16String::from_utf16(id);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-mode
TextTrackMode TextTrack::mode()
{
    return m_mode;
}

void TextTrack::set_mode(TextTrackMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;

    if (m_mode == TextTrackMode::Disabled)
        unset_active_flag_of_cues();

    if (m_media_element)
        m_media_element->text_track_mode_changed();

    // FIXME: Update activeSourceBuffers for tracks from a SourceBuffer:
    //        https://w3c.github.io/media-source/#active-source-buffer-changes
}

// https://html.spec.whatwg.org/multipage/media.html#text-track-cue-active-flag
void TextTrack::unset_active_flag_of_cues()
{
    // The user agent must synchronously unset this flag whenever the text track cue is removed from its text track's
    // text track list of cues; whenever the text track itself is removed from its media element's list of text tracks
    // or has its text track mode changed to disabled; and whenever the media element's readyState is changed back to
    // HAVE_NOTHING.
    // FIXME: When the flag is unset in this way for one or more cues in text tracks that were showing prior to the
    //        relevant incident, the user agent must, after having unset the flag for all the affected cues, apply the
    //        rules for updating the text track rendering of those text tracks.
    for (auto const& cue : m_text_track_list_of_cues)
        cue->set_active(false);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttrack-oncuechange
void TextTrack::set_oncuechange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::cuechange, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttrack-oncuechange
WebIDL::CallbackType* TextTrack::oncuechange()
{
    return event_handler_attribute(HTML::EventNames::cuechange);
}

void TextTrack::set_readiness_state(ReadinessState readiness_state)
{
    m_readiness_state = readiness_state;

    for (auto observer : m_observers) {
        if (auto callback = observer->track_readiness_observer())
            callback->function()(m_readiness_state);
    }
}

void TextTrack::register_observer(Badge<TextTrackObserver>, TextTrackObserver& observer)
{
    auto result = m_observers.set(observer);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void TextTrack::unregister_observer(Badge<TextTrackObserver>, TextTrackObserver& observer)
{
    bool was_removed = m_observers.remove(observer);
    VERIFY(was_removed);
}

TextTrackKind text_track_kind_from_string(String value)
{
    // https://html.spec.whatwg.org/multipage/media.html#attr-track-kind

    if (value.is_empty() || value.equals_ignoring_ascii_case("subtitles"sv)) {
        return TextTrackKind::Subtitles;
    }
    if (value.equals_ignoring_ascii_case("captions"sv)) {
        return TextTrackKind::Captions;
    }
    if (value.equals_ignoring_ascii_case("descriptions"sv)) {
        return TextTrackKind::Descriptions;
    }
    if (value.equals_ignoring_ascii_case("chapters"sv)) {
        return TextTrackKind::Chapters;
    }
    if (value.equals_ignoring_ascii_case("metadata"sv)) {
        return TextTrackKind::Metadata;
    }

    return TextTrackKind::Metadata;
}

}
