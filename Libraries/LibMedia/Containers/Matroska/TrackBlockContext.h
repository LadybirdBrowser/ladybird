/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/Export.h>

namespace Media::Matroska {

class TrackEntry;

struct TrackBlockContext {
    CodecID codec_id { CodecID::Unknown };
    double timestamp_scale { 1 };
    u64 codec_delay { 0 };
    u64 seek_pre_roll { 0 };
    i64 timestamp_offset { 0 };
    u64 default_duration { 0 };

    static MEDIA_API TrackBlockContext from_track_entry(TrackEntry const&);
};

using TrackBlockContexts = HashMap<u64, TrackBlockContext>;

}
