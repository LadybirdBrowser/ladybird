/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <LibAudio/PlaybackGroupId.h>

namespace AudioServer {

// Which groups of which tabs capture, for the Browser to show. A group captures from the moment a stream of it may, and
// whether the device delivers anything or not, until its last stream is gone.
class CaptureActivity {
    AK_MAKE_NONCOPYABLE(CaptureActivity);
    AK_MAKE_NONMOVABLE(CaptureActivity);

public:
    using Report = Function<void(u64 tab_id, Audio::PlaybackGroupId, bool capturing)>;

    explicit CaptureActivity(Report);

    void stream_started(u64 tab_id, Audio::PlaybackGroupId);
    void stream_stopped(u64 tab_id, Audio::PlaybackGroupId);

    size_t stream_count_for_testing(u64 tab_id, Audio::PlaybackGroupId) const;

private:
    Report m_report;
    // A group is here, with the number of its streams, exactly while the Browser has been told it captures.
    HashMap<u64, HashMap<Audio::PlaybackGroupId, size_t>> m_stream_counts;
};

}
