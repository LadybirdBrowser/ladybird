/*
 * Copyright (c) 2024-2025, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/VTTCue.h>
#include <LibWeb/HTML/TextTrackCue.h>

namespace Web::WebVTT {

// https://w3c.github.io/webvtt/#vttcue
class VTTCue final : public HTML::TextTrackCue {
    WEB_RUST_INTERFACE(VTTCue, vtt_cue)

public:
    // https://w3c.github.io/webvtt/#dom-vttcue-vttcue
    // NB: The end time must be neither negative infinity nor NaN.
    static GC::Ref<VTTCue> create(double start_time, double end_time, Utf16String const& text);
};

}
