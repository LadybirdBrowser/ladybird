/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/TimeRanges.h>

namespace Media {

class ContainerNavigator {
public:
    virtual ~ContainerNavigator() = default;
    virtual TimeRanges buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const = 0;
};

}
