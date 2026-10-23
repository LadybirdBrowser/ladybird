/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/WeakPtr.h>
#include <LibAudio/Audibility.h>
#include <LibAudio/PlaybackGroupId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Which playback groups a process's AudioServer has recently heard. Only the watched AudioServer is heard: what it heard
// is forgotten once it dies or another is watched instead, and one that has been replaced is heard from no more.
class WEBVIEW_API AudioServerAudibility {
public:
    using OnChange = Function<void(Audio::PlaybackGroupId, Audio::Audibility)>;
    // Whether to take the AudioServer's report.
    using Filter = Function<bool(u64 tab_id, Audio::PlaybackGroupId, Audio::Audibility)>;

    explicit AudioServerAudibility(OnChange);
    ~AudioServerAudibility();

    void watch(AudioServerControlClient&, Filter);
    void forget(Audio::PlaybackGroupId);

private:
    void stop_watching();
    void set(Audio::PlaybackGroupId, Audio::Audibility);
    void forget_everything();

    OnChange m_on_change;
    WeakPtr<AudioServerControlClient> m_controller;
    HashTable<Audio::PlaybackGroupId> m_audible_groups;
};

}
