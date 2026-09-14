/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>

namespace Web::WebAudio::Rendering {

void radix2_fft(Span<float> re, Span<float> im);

}
