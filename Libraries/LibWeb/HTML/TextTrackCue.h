/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TextTrackCue.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/media.html#texttrackcue
class TextTrackCue : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TextTrackCue, text_track_cue)

public:
    GC::Ref<DOM::EventTarget> event_target() const;

    GC::Ptr<TextTrack> track() const;
    void set_track(GC::Ptr<TextTrack>);

    Utf16String id() const;
    void set_id(Utf16String const&);

    double start_time() const;
    double end_time() const;
    bool pause_on_exit() const;

    bool is_active() const;
    void set_active(bool);

    u64 order_added() const;
    void set_order_added(u64);
};

}
