/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/CaptureActivity.h>

namespace AudioServer {

CaptureActivity::CaptureActivity(Report report)
    : m_report(move(report))
{
}

void CaptureActivity::stream_started(u64 tab_id, Audio::PlaybackGroupId group)
{
    auto& stream_count = m_stream_counts.ensure(tab_id).ensure(group, [] { return 0uz; });
    if (stream_count++ == 0)
        m_report(tab_id, group, true);
}

void CaptureActivity::stream_stopped(u64 tab_id, Audio::PlaybackGroupId group)
{
    auto groups = m_stream_counts.find(tab_id);
    if (groups == m_stream_counts.end())
        return;
    auto stream_count = groups->value.find(group);
    if (stream_count == groups->value.end())
        return;
    if (--stream_count->value > 0)
        return;
    groups->value.remove(stream_count);
    if (groups->value.is_empty())
        m_stream_counts.remove(groups);
    m_report(tab_id, group, false);
}

size_t CaptureActivity::stream_count_for_testing(u64 tab_id, Audio::PlaybackGroupId group) const
{
    auto groups = m_stream_counts.find(tab_id);
    if (groups == m_stream_counts.end())
        return 0;
    return groups->value.get(group).value_or(0);
}

}
