/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/TimeRanges.h>

namespace Media {

struct SeekSkipped { };

struct SeekedPosition {
    i64 byte_position;
    AK::Duration timestamp;
};

using SeekResult = Variant<Empty, SeekSkipped, SeekedPosition>;

class ContainerNavigator {
public:
    virtual ~ContainerNavigator() = default;
    virtual DecoderErrorOr<SeekResult> seek_to_timestamp(AK::Duration) const { return Empty {}; }
    virtual TimeRanges buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const = 0;
};

}
