/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Size.h>
#include <LibImageDecoderClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/Application.h>

namespace Ladybird {

class HeadlessWebView;

class Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    static Application& the()
    {
        return static_cast<Application&>(WebView::Application::the());
    }

    virtual void create_platform_arguments(Core::ArgsParser&) override;
    virtual void create_platform_options(WebView::ChromeOptions&, WebView::WebContentOptions&) override;

    ErrorOr<void> launch_services();

    static Requests::RequestClient& request_client() { return *the().m_request_client; }
    static ImageDecoderClient::Client& image_decoder_client() { return *the().m_image_decoder_client; }

    HeadlessWebView& create_web_view(Core::AnonymousBuffer theme, Gfx::IntSize window_size);
    HeadlessWebView& create_child_web_view(HeadlessWebView const&, u64 page_index);
    void destroy_web_views();

    template<typename Callback>
    void for_each_web_view(Callback&& callback)
    {
        for (auto& web_view : m_web_views)
            callback(*web_view);
    }

    int screenshot_timeout { 1 };
    ByteString resources_folder;
    bool dump_failed_ref_tests { false };
    bool dump_layout_tree { false };
    bool dump_text { false };
    bool dump_gc_graph { false };
    bool is_layout_test_mode { false };
    size_t test_concurrency { 1 };
    ByteString test_root_path;
    ByteString test_glob;
    bool test_dry_run { false };
    bool rebaseline { false };
    bool log_slowest_tests { false };
    int per_test_timeout_in_seconds { 30 };

private:
    RefPtr<Requests::RequestClient> m_request_client;
    RefPtr<ImageDecoderClient::Client> m_image_decoder_client;

    Vector<NonnullOwnPtr<HeadlessWebView>> m_web_views;
};

}
