/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/kmalloc.h>
#include <LibWebCommon/MediaSession/MediaSessionState.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Export.h>

namespace WebView {

struct MediaSessionSource {
    u64 view_id;
    Web::PageId page_id;
    bool is_private { false };

    bool operator==(MediaSessionSource const&) const = default;
};

using NowPlayingState = Web::MediaSession::MediaSessionState;

WEBVIEW_API bool same_artwork(RefPtr<Gfx::Bitmap const> const&, RefPtr<Gfx::Bitmap const> const&);

class WEBVIEW_API MediaSessionPlatformBackend {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~MediaSessionPlatformBackend() = default;
    virtual void update(Optional<NowPlayingState> const&) = 0;

    Function<void(Web::Bindings::MediaSessionAction, Optional<double>, Optional<double>, Optional<bool>)> on_action;
};

class WEBVIEW_API MediaSessionCoordinator {
public:
    void update(MediaSessionSource, Optional<Web::MediaSession::MediaSessionState>, String fallback_title);
    void remove(MediaSessionSource);
    void remove_view(u64 view_id);
    void set_fallback_title(u64 view_id, String const&);
    void set_platform_backend(OwnPtr<MediaSessionPlatformBackend>);

    Optional<NowPlayingState> const& now_playing_state() const { return m_now_playing_state; }
    void handle_action(Web::Bindings::MediaSessionAction, Optional<double> seek_offset = {}, Optional<double> seek_time = {}, Optional<bool> fast_seek = {});

    // The state, and the action routing, that the view would get if it were the only one with a media session.
    Optional<NowPlayingState> state_for_view(u64 view_id) const;
    void handle_action_for_view(u64 view_id, Web::Bindings::MediaSessionAction, Optional<double> seek_offset = {}, Optional<double> seek_time = {}, Optional<bool> fast_seek = {});
    Function<void(MediaSessionSource, Web::Bindings::MediaSessionAction, Optional<double>, Optional<double>, Optional<bool>)> on_action;

private:
    struct SourceState {
        MediaSessionSource source;
        Web::MediaSession::MediaSessionState state;
        String fallback_title;
        u64 activation_sequence;
    };

    void publish();
    SourceState const* select_source(Optional<u64> view_id) const;
    static NowPlayingState now_playing_state_for(SourceState const&);
    void send_action(SourceState const&, Web::Bindings::MediaSessionAction, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek);
    Vector<SourceState> m_sources;
    u64 m_activation_sequence { 0 };
    Optional<NowPlayingState> m_now_playing_state;
    OwnPtr<MediaSessionPlatformBackend> m_backend;
};

}
