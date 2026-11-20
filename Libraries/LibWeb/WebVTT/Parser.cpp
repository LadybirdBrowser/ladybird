/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebVTT/Parser.h>

namespace Web::WebVTT {

extern "C" {

// Defined in Bindings/Rust/src/web_vtt/exports.rs.
bool web_vtt_parse_webvtt_file(u8 const* bytes, size_t length, void* ctx, void (*on_cue)(void* ctx, FlatPtr identifier, double start_time, double end_time, FlatPtr text));
}

// https://w3c.github.io/webvtt/#webvtt-parser
Optional<Vector<ParsedCue>> parse_webvtt_file(ReadonlyBytes bytes)
{
    Vector<ParsedCue> cues;
    auto on_cue = [](void* ctx, FlatPtr identifier, double start_time, double end_time, FlatPtr text) {
        static_cast<Vector<ParsedCue>*>(ctx)->append({
            .identifier = Utf16String::adopt_raw(identifier),
            .start_time = start_time,
            .end_time = end_time,
            .text = Utf16String::adopt_raw(text),
        });
    };
    if (!web_vtt_parse_webvtt_file(bytes.data(), bytes.size(), &cues, on_cue))
        return {};
    return cues;
}

}
