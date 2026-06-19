/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/SiteIsolationManager.h>

namespace WebView {

SiteIsolationManager& SiteIsolationManager::the()
{
    static auto& manager = *new SiteIsolationManager;
    return manager;
}

void SiteIsolationManager::did_create_child_frame(u64 page_id, String parent_frame_id, String frame_id)
{
    auto& child_frames = m_child_frames.ensure(page_id, [] {
        return HashMap<String, ChildFrameHost> {};
    });
    ChildFrameHost child_frame;
    child_frame.parent_frame_id = move(parent_frame_id);
    child_frames.set(move(frame_id), move(child_frame));
}

void SiteIsolationManager::did_update_child_frame_viewport(u64 page_id, String frame_id, Web::DevicePixelRect viewport_rect, double device_pixel_ratio)
{
    auto child_frame = this->child_frame(page_id, frame_id);
    if (!child_frame.has_value())
        return;

    child_frame->viewport_rect = viewport_rect;
    child_frame->device_pixel_ratio = device_pixel_ratio;
}

void SiteIsolationManager::did_commit_child_frame_navigation(u64 page_id, String frame_id, URL::URL url)
{
    auto child_frame = this->child_frame(page_id, frame_id);
    if (!child_frame.has_value())
        return;

    child_frame->last_committed_url = move(url);
}

void SiteIsolationManager::did_destroy_child_frame(u64 page_id, StringView frame_id)
{
    auto child_frames = m_child_frames.get(page_id);
    if (!child_frames.has_value())
        return;

    child_frames->remove(frame_id);
    if (child_frames->is_empty())
        m_child_frames.remove(page_id);
}

void SiteIsolationManager::remove_page(u64 page_id)
{
    m_child_frames.remove(page_id);
}

Optional<SiteIsolationManager::ChildFrameHost&> SiteIsolationManager::child_frame(u64 page_id, StringView frame_id)
{
    auto child_frames = m_child_frames.get(page_id);
    if (!child_frames.has_value())
        return {};

    return child_frames->get(frame_id);
}

Optional<SiteIsolationManager::ChildFrameHost const&> SiteIsolationManager::child_frame(u64 page_id, StringView frame_id) const
{
    auto child_frames = m_child_frames.get(page_id);
    if (!child_frames.has_value())
        return {};

    return child_frames->get(frame_id);
}

}
