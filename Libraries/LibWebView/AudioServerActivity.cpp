/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/AudioServerActivity.h>
#include <LibWebView/AudioServerControlClient.h>

namespace WebView {

AudioServerActivity::AudioServerActivity(OnAudibilityChange on_audibility_change, OnCaptureChange on_capture_change)
    : m_on_audibility_change(move(on_audibility_change))
    , m_on_capture_change(move(on_capture_change))
{
}

AudioServerActivity::~AudioServerActivity()
{
    stop_watching();
}

void AudioServerActivity::watch(AudioServerControlClient& controller, AudibilityFilter audibility_filter, CaptureFilter capture_filter)
{
    stop_watching();
    forget_everything();
    m_controller = controller.make_weak_ptr<AudioServerControlClient>();
    controller.on_playback_group_audibility_changed = [this, filter = move(audibility_filter)](u64 tab_id, Audio::PlaybackGroupId group, Audio::Audibility audibility) {
        if (filter(tab_id, group, audibility))
            set_audibility(group, audibility);
    };
    controller.on_capture_activity_changed = [this, filter = move(capture_filter)](u64 tab_id, Audio::PlaybackGroupId group, bool capturing) {
        if (filter(tab_id))
            set_capturing(group, capturing);
    };
    controller.on_death = [this] {
        forget_everything();
    };
}

void AudioServerActivity::forget_audibility(Audio::PlaybackGroupId group)
{
    set_audibility(group, Audio::Audibility::Inaudible);
}

void AudioServerActivity::stop_watching()
{
    // A replaced AudioServer's death can be noticed after its replacement has begun reporting.
    if (auto controller = m_controller.strong_ref()) {
        controller->on_playback_group_audibility_changed = nullptr;
        controller->on_capture_activity_changed = nullptr;
        controller->on_death = nullptr;
    }
    m_controller.clear();
}

void AudioServerActivity::set_audibility(Audio::PlaybackGroupId group, Audio::Audibility audibility)
{
    switch (audibility) {
    case Audio::Audibility::Audible:
        if (m_audible_groups.set(group) != HashSetResult::InsertedNewEntry)
            return;
        break;
    case Audio::Audibility::Inaudible:
        if (!m_audible_groups.remove(group))
            return;
        break;
    }
    m_on_audibility_change(group, audibility);
}

void AudioServerActivity::set_capturing(Audio::PlaybackGroupId group, bool capturing)
{
    auto changed = capturing ? m_capturing_groups.set(group) == HashSetResult::InsertedNewEntry : m_capturing_groups.remove(group);
    if (changed)
        m_on_capture_change();
}

void AudioServerActivity::forget_everything()
{
    for (auto group : exchange(m_audible_groups, {}))
        m_on_audibility_change(group, Audio::Audibility::Inaudible);
    if (!exchange(m_capturing_groups, {}).is_empty())
        m_on_capture_change();
}

}
