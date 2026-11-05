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

// Which groups a process's AudioServer has recently heard, and which hold capture streams. Only the watched AudioServer
// is heard: what it reported is forgotten once it dies or another is watched instead, and one that has been replaced is
// heard from no more.
class WEBVIEW_API AudioServerActivity {
public:
    using OnAudibilityChange = Function<void(Audio::PlaybackGroupId, Audio::Audibility)>;
    using OnCaptureChange = Function<void()>;
    // Whether to take the AudioServer's report.
    using AudibilityFilter = Function<bool(u64 tab_id, Audio::PlaybackGroupId, Audio::Audibility)>;
    using CaptureFilter = Function<bool(u64 tab_id)>;

    AudioServerActivity(OnAudibilityChange, OnCaptureChange);
    ~AudioServerActivity();

    void watch(AudioServerControlClient&, AudibilityFilter, CaptureFilter);
    void forget_audibility(Audio::PlaybackGroupId);

    // The renderer labels its capture streams, so these name the pages it says it captures for.
    HashTable<Audio::PlaybackGroupId> const& capturing_groups() const { return m_capturing_groups; }
    void set_capturing_for_testing(Audio::PlaybackGroupId group, bool capturing) { set_capturing(group, capturing); }

private:
    void stop_watching();
    void set_audibility(Audio::PlaybackGroupId, Audio::Audibility);
    void set_capturing(Audio::PlaybackGroupId, bool);
    void forget_everything();

    OnAudibilityChange m_on_audibility_change;
    OnCaptureChange m_on_capture_change;
    WeakPtr<AudioServerControlClient> m_controller;
    HashTable<Audio::PlaybackGroupId> m_audible_groups;
    HashTable<Audio::PlaybackGroupId> m_capturing_groups;
};

}
