/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API SiteIsolationManager {
public:
    static SiteIsolationManager& the();

    struct RemoteChildFrameInputTarget {
        RefPtr<WebContentClient> remote_client;
        u64 remote_page_id { 0 };
        Web::DevicePixelRect viewport_rect;
    };

    Web::NavigationProcessDecision decide_navigation_process(WebContentClient&, u64 page_id, Optional<String> frame_id, URL::URL current_url, URL::URL target_url, Web::NavigationTarget);

    void transition_child_frame_to_remote(WebContentClient& parent_client, u64 page_id, StringView frame_id, NonnullRefPtr<WebContentClient>, u64 remote_page_id);
    void transition_child_frame_to_local(CanonicalNavigable&);

    void remove_child_frame_subtree(CanonicalNavigable&);

    void remove_page(WebContentClient&, u64 page_id);
    void remove_all_pages_for_client(WebContentClient&);

    Optional<RemoteChildFrameInputTarget> remote_child_frame_input_target_at(WebContentClient&, u64 page_id, Web::DevicePixelPoint) const;
    String dump_process_tree(WebContentClient&, u64 page_id) const;
    HashMap<pid_t, pid_t> remote_frame_process_embedders() const;

private:
    SiteIsolationManager() = default;
};

}
