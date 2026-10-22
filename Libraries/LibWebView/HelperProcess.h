/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibIPC/TransportHandle.h>
#include <LibMediaClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>
#include <RequestServer/SiteBinding.h>

namespace WebView {

WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(IsPrivate, Web::PageId initial_page_id, Web::HTML::CrossProcessId root_navigable_id);

WEBVIEW_API ErrorOr<NonnullRefPtr<MediaClient::Client>> launch_media_server_process();
WEBVIEW_API ErrorOr<NonnullRefPtr<AudioServerControlClient>> launch_audio_server_process();
WEBVIEW_API ErrorOr<NonnullRefPtr<VideoDecoderServerControlClient>> launch_video_decoder_server_process(Optional<Gfx::GpuDeviceIdentity> const&);
WEBVIEW_API void shut_down_video_decoder_servers();
WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::CompositorClient>> launch_compositor_process();
WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::WebWorkerClient>> launch_web_worker_process(Web::HTML::AgentType, IsPrivate, Web::HTML::WorkerAgentId);
// Serves the given site's partitions, or all partitions if site is empty. With sites_have_their_own,
// the server with no site only handles site requests in transit to their site's RequestServer.
WEBVIEW_API ErrorOr<NonnullRefPtr<Requests::RequestControlClient>> launch_request_server_process(ByteString const& cache_path, Optional<Utf16String> const& site, bool sites_have_their_own, HTTPDiskCacheMode);
WEBVIEW_API void apply_dns_settings(Requests::RequestControlClient&);

struct ImageDecoderConnection {
    IPC::TransportHandle handle;
    u64 child_id { 0 };
};

// Launches an ImageDecoder that serves a single renderer or worker, which talks to it over the returned handle.
WEBVIEW_API ErrorOr<ImageDecoderConnection> launch_image_decoder_process();

// Hands the client its ImageDecoder. If that decoder dies while the client is still connected, the client gets a new one.
WEBVIEW_API void connect_to_image_decoder(WebContentClient&, ImageDecoderConnection);
WEBVIEW_API void connect_to_image_decoder(WebWorkerClient&, ImageDecoderConnection);

struct RequestServerClientConnection {
    IPC::TransportHandle handle;
    int client_id { -1 };
};

// A new client of the session's RequestServer with no site. The client uses the cookies of the given session, which
// must be the session of the process the client is for.
WEBVIEW_API ErrorOr<RequestServerClientConnection> connect_new_request_server_client(BrowsingSession&, RequestServer::SiteBinding);
// Names a tab's mix in the AudioServer. Every renderer and worker process gets one of its own, and every process
// that plays on its behalf plays into the same mix.
WEBVIEW_API u64 allocate_audio_tab_id();
// Launches the AudioServer for a renderer if it has none, keeping its controller connection in the given slot.
WEBVIEW_API ErrorOr<void> ensure_audio_server_is_running(RefPtr<AudioServerControlClient>& controller);
// Likewise, and connects a new client to it as a member of the given tab's mix.
WEBVIEW_API ErrorOr<IPC::TransportHandle> connect_new_audio_server_client(RefPtr<AudioServerControlClient>& controller, u64 audio_tab_id);
// Launches the MediaServer for a renderer if it has none, keeping its controller connection in the given slot, and
// connects a new client to it. The server gets its AudioServer connection through connect_audio_server, and its
// VideoDecoderServer from the Browser, which launches one for it on request.
struct MediaServerClientConnection {
    IPC::TransportHandle handle;
    int client_id { -1 };
};
WEBVIEW_API ErrorOr<MediaServerClientConnection> connect_new_media_server_client(RefPtr<MediaClient::Client>& controller, Function<ErrorOr<IPC::TransportHandle>()> connect_audio_server);

}
