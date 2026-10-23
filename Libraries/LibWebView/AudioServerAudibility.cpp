/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/AudioServerAudibility.h>
#include <LibWebView/AudioServerControlClient.h>

namespace WebView {

AudioServerAudibility::AudioServerAudibility(OnChange on_change)
    : m_on_change(move(on_change))
{
}

AudioServerAudibility::~AudioServerAudibility()
{
    stop_watching();
}

void AudioServerAudibility::watch(AudioServerControlClient& controller, Filter filter)
{
    stop_watching();
    forget_everything();
    m_controller = controller.make_weak_ptr<AudioServerControlClient>();
    controller.on_playback_group_audibility_changed = [this, filter = move(filter)](u64 tab_id, Audio::PlaybackGroupId group, Audio::Audibility audibility) {
        if (filter(tab_id, group, audibility))
            set(group, audibility);
    };
    controller.on_death = [this] {
        forget_everything();
    };
}

void AudioServerAudibility::forget(Audio::PlaybackGroupId group)
{
    set(group, Audio::Audibility::Inaudible);
}

void AudioServerAudibility::stop_watching()
{
    // A replaced AudioServer's death can be noticed after its replacement has begun reporting.
    if (auto controller = m_controller.strong_ref()) {
        controller->on_playback_group_audibility_changed = nullptr;
        controller->on_death = nullptr;
    }
    m_controller.clear();
}

void AudioServerAudibility::set(Audio::PlaybackGroupId group, Audio::Audibility audibility)
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
    m_on_change(group, audibility);
}

void AudioServerAudibility::forget_everything()
{
    for (auto group : exchange(m_audible_groups, {}))
        m_on_change(group, Audio::Audibility::Inaudible);
}

}
