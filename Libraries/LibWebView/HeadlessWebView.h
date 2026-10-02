/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibCore/Forward.h>
#include <LibCore/Timer.h>
#include <LibGfx/Forward.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

class WEBVIEW_API HeadlessWebView : public WebView::ViewImplementation {
public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<HeadlessWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size, IsPrivate = IsPrivate::No);
    static NonnullOwnPtr<HeadlessWebView> create_child(HeadlessWebView&, CanonicalTraversable&);
    static NonnullOwnPtr<PictureInPictureWindow> create_picture_in_picture_window(HeadlessWebView& requesting_view, CanonicalTraversable&, Gfx::IntSize video_size);

    void reset_viewport_size(Web::DevicePixelSize);

    void close_child_web_views()
    {
        for (auto& child : m_child_web_views) {
            child->close_child_web_views();
            // Children sharing a crashed WebContent process are discarded by their pending crash callbacks.
            if (!child->handle().is_empty() && child->page().is_open()) {
                child->request_close();
                child->schedule_forced_close();
            }
        }
    }

    void disconnect_child_crash_handlers()
    {
        // Disconnect crash handlers so child crashes don't propagate to parent.
        // We don't destroy the children because there may be pending deferred_invokes
        // that would cause use-after-free.
        for (auto& child : m_child_web_views) {
            child->m_propagate_crashes_to_parent = false;
            child->disconnect_child_crash_handlers();
        }
    }

protected:
    HeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, IsPrivate = IsPrivate::No);

    void propagate_web_content_crash(WebContentCrashReason);
    HeadlessWebView& adopt_child_web_view(NonnullOwnPtr<HeadlessWebView>);
    void discard_child_web_view(HeadlessWebView&);
    void schedule_forced_close();
    void prepare_page_for_tab(WebContentPage&) override;
    void update_zoom() override;
    ViewImplementation* create_view_for_new_tab_or_window(IsPrivate) override;

    virtual Web::DevicePixelSize viewport_size() const override { return m_viewport_size; }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }

    Core::AnonymousBuffer m_theme;
    Web::DevicePixelSize m_viewport_size;

    Web::PendingDialog m_pending_dialog { Web::PendingDialog::None };
    Optional<Utf16String> m_pending_prompt_text;

    // When restoring from fullscreen, we need to know to what dimension.
    Web::DevicePixelRect m_previous_dimensions;

    RefPtr<Core::Timer> m_forced_close_timer;
    WeakPtr<HeadlessWebView> m_parent_web_view;
    bool m_propagate_crashes_to_parent { true };
    Vector<NonnullOwnPtr<HeadlessWebView>> m_child_web_views;
};

}
