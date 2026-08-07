/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/Document.h>
#include <LibMedia/Containers/Matroska/TrackBlockContext.h>
#include <LibMedia/Containers/Matroska/Utilities.h>

namespace Media::Matroska {

TrackBlockContext TrackBlockContext::from_track_entry(TrackEntry const& entry)
{
    return {
        .codec_id = codec_id_from_matroska_track_entry(entry),
        .timestamp_scale = entry.timestamp_scale(),
        .codec_delay = entry.codec_delay(),
        .seek_pre_roll = entry.seek_pre_roll(),
        .timestamp_offset = entry.timestamp_offset(),
        .default_duration = entry.default_duration(),
    };
}

}
