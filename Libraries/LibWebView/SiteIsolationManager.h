/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API SiteIsolationManager {
public:
    static SiteIsolationManager& the();

    struct ChildFrameHost {
        String parent_frame_id;
        Optional<URL::URL> last_committed_url;
        Optional<Web::DevicePixelRect> viewport_rect;
        double device_pixel_ratio { 1 };
    };

    void did_create_child_frame(u64 page_id, String parent_frame_id, String frame_id);
    void did_update_child_frame_viewport(u64 page_id, String frame_id, Web::DevicePixelRect viewport_rect, double device_pixel_ratio);
    void did_commit_child_frame_navigation(u64 page_id, String frame_id, URL::URL url);
    void did_destroy_child_frame(u64 page_id, StringView frame_id);
    void remove_page(u64 page_id);

    Optional<ChildFrameHost&> child_frame(u64 page_id, StringView frame_id);
    Optional<ChildFrameHost const&> child_frame(u64 page_id, StringView frame_id) const;

    template<CallableAs<IterationDecision, String const&, ChildFrameHost const&> Callback>
    void for_each_child_frame(u64 page_id, Callback callback) const;

private:
    SiteIsolationManager() = default;

    HashMap<u64, HashMap<String, ChildFrameHost>> m_child_frames;
};

template<CallableAs<IterationDecision, String const&, SiteIsolationManager::ChildFrameHost const&> Callback>
void SiteIsolationManager::for_each_child_frame(u64 page_id, Callback callback) const
{
    auto child_frames = m_child_frames.get(page_id);
    if (!child_frames.has_value())
        return;

    for (auto const& entry : *child_frames) {
        if (callback(entry.key, entry.value) == IterationDecision::Break)
            return;
    }
}

}
