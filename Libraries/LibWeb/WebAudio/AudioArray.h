/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>

namespace Web::WebAudio {

// Utilities for moving sample data in and out of the Float32Arrays exposed by the Web Audio API. A Float32Array's
// contents can change from JavaScript at any time, so audio data destined for the rendering thread is always snapshotted
// into an owned buffer first.

size_t float32_array_length(JS::Float32Array const&);
Vector<float> copy_float32_array(JS::Float32Array const&);
void overwrite_float32_array(JS::Float32Array&, ReadonlySpan<float>);

}
