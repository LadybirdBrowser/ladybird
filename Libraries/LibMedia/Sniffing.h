/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/IncrementallyPopulatedStream.h>

namespace Media {

bool sniff_webm(IncrementallyPopulatedStream::Cursor&);
bool sniff_mp4(IncrementallyPopulatedStream::Cursor&);

}
