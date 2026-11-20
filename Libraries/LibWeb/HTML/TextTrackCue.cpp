/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TextTrackCue.h>

namespace Web::HTML {

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void* web_text_track_cue_event_target(void* cue);
void* web_text_track_cue_track(void* cue);
void web_text_track_cue_set_track(void* cue, void* track);
FlatPtr web_text_track_cue_id(void* cue);
void web_text_track_cue_set_id(void* cue, FlatPtr id);
double web_text_track_cue_start_time(void* cue);
double web_text_track_cue_end_time(void* cue);
bool web_text_track_cue_pause_on_exit(void* cue);
bool web_text_track_cue_is_active(void* cue);
void web_text_track_cue_set_active(void* cue, bool active);
u64 web_text_track_cue_order_added(void* cue);
void web_text_track_cue_set_order_added(void* cue, u64 order_added);

// Declared in Bindings/Rust/src/html/text_track_cue.rs.
void web_html_text_track_cue_timing_changed(void* track);
}

static void* to_abi(TextTrackCue const* cue)
{
    return const_cast<TextTrackCue*>(cue);
}

static TextTrack& track_from_abi(void* track)
{
    return *static_cast<TextTrack*>(static_cast<GC::Cell*>(track));
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-starttime
// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-endtime
void web_html_text_track_cue_timing_changed(void* track)
{
    auto& text_track = track_from_abi(track);
    text_track.sort_text_track_list_of_cues();

    // ...if the TextTrackCue object's text track cue is in a text track's list of cues, and that text track is in a media
    // element's list of text tracks, and the media element's show poster flag is not set, then run the time marches on
    // steps for that media element.
    if (auto media_element = text_track.media_element(); media_element && !media_element->show_poster())
        media_element->time_marches_on(HTMLMediaElement::TimeMarchesOnReason::Other);
}

GC::Ref<DOM::EventTarget> TextTrackCue::event_target() const
{
    return *static_cast<DOM::EventTarget*>(static_cast<GC::Cell*>(web_text_track_cue_event_target(to_abi(this))));
}

GC::Ptr<TextTrack> TextTrackCue::track() const
{
    auto* track = web_text_track_cue_track(to_abi(this));
    if (!track)
        return nullptr;
    return track_from_abi(track);
}

void TextTrackCue::set_track(GC::Ptr<TextTrack> track)
{
    web_text_track_cue_set_track(to_abi(this), track ? static_cast<GC::Cell*>(track.ptr()) : nullptr);
}

Utf16String TextTrackCue::id() const
{
    return Utf16String::adopt_raw(web_text_track_cue_id(to_abi(this)));
}

void TextTrackCue::set_id(Utf16String const& id)
{
    web_text_track_cue_set_id(to_abi(this), id.raw_identity());
}

double TextTrackCue::start_time() const
{
    return web_text_track_cue_start_time(to_abi(this));
}

double TextTrackCue::end_time() const
{
    return web_text_track_cue_end_time(to_abi(this));
}

bool TextTrackCue::pause_on_exit() const
{
    return web_text_track_cue_pause_on_exit(to_abi(this));
}

bool TextTrackCue::is_active() const
{
    return web_text_track_cue_is_active(to_abi(this));
}

void TextTrackCue::set_active(bool active)
{
    web_text_track_cue_set_active(to_abi(this), active);
}

u64 TextTrackCue::order_added() const
{
    return web_text_track_cue_order_added(to_abi(this));
}

void TextTrackCue::set_order_added(u64 order_added)
{
    web_text_track_cue_set_order_added(to_abi(this), order_added);
}

}
