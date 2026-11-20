/*
 * Copyright (c) 2024-2025, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebVTT/VTTCue.h>

namespace Web::WebVTT {

extern "C" {

// Defined in Bindings/Rust/src/web_vtt/exports.rs.
void* web_vtt_cue_create(JSVM*, double start_time, double end_time, FlatPtr text);
}

GC::Ref<VTTCue> VTTCue::create(double start_time, double end_time, Utf16String const& text)
{
    auto* cue = web_vtt_cue_create(JS::HostABI::vm_to_abi(JS::VM::the()), start_time, end_time, text.raw_identity());
    return GC::Ref { *static_cast<VTTCue*>(cue) };
}

}
