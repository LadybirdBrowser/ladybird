/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/MediaElementAudioSourceRenderNode.h>

namespace Web::WebAudio::Render {

// Tradeoffs: This is currently a type alias for MediaElementAudioSourceRenderNode.
// That keeps MediaStreamAudioSourceNode working with the same provider and resampling path,
// but it also means we do not yet enforce capture-specific latency, channel caps, or drift handling.
// Splitting the implementation later will require revisiting render node state and tuning.
using MediaStreamAudioSourceRenderNode = MediaElementAudioSourceRenderNode;

}
