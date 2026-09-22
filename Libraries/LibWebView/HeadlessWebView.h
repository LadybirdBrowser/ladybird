/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibCompositing/PageId.h>
#include <LibCompositing/PixelUnits.h>
#include <LibCore/Forward.h>
#include <LibCore/Timer.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Page/Page.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

class WEBVIEW_API HeadlessWebView : public WebView::ViewImplementation {
public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<HeadlessWebView> create(Core::AnonymousBuffer theme, Compositing::DevicePixelSize window_size, IsPrivate = IsPrivate::No);
    static NonnullOwnPtr<HeadlessWebView> create_child(HeadlessWebView&, WebContentClient& page_process, Compositing::PageId page_index);

    void reset_viewport_size(Compositing::DevicePixelSize);

    void close_child_web_views()
    {
        for (auto& child : m_child_web_views) {
            child->close_child_web_views();
            // Children sharing a crashed WebContent process are discarded by their pending crash callbacks.
            if (!child->handle().is_empty() && child->client().is_open()) {
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
    HeadlessWebView(Core::AnonymousBuffer theme, Compositing::DevicePixelSize viewport_size, IsPrivate = IsPrivate::No);

    void propagate_web_content_crash(WebContentCrashReason);
    void discard_child_web_view(HeadlessWebView&);
    void schedule_forced_close();
    void initialize_client(CreateNewClient, Optional<Web::HTML::CrossProcessId> initial_document_state_id = {}) override;
    void update_zoom() override;

    virtual Compositing::DevicePixelSize viewport_size() const override { return m_viewport_size; }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }

    Core::AnonymousBuffer m_theme;
    Compositing::DevicePixelSize m_viewport_size;

    Web::Page::PendingDialog m_pending_dialog { Web::Page::PendingDialog::None };
    Optional<Utf16String> m_pending_prompt_text;

    // When restoring from fullscreen, we need to know to what dimension.
    Compositing::DevicePixelRect m_previous_dimensions;

    RefPtr<Core::Timer> m_forced_close_timer;
    WeakPtr<HeadlessWebView> m_parent_web_view;
    bool m_propagate_crashes_to_parent { true };
    Vector<NonnullOwnPtr<HeadlessWebView>> m_child_web_views;
};

}
