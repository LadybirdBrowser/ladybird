/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/Swift.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Forward.h>
#include <LibImageDecoderClient/Client.h>
#include <LibMain/Main.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/URL.h>
#include <LibWebView/Options.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessManager.h>

namespace WebView {

class Application {
    AK_MAKE_NONCOPYABLE(Application);

public:
    virtual ~Application();

    int execute();

    static Application& the() { return *s_the; }

    static ChromeOptions const& chrome_options() { return the().m_chrome_options; }
    static WebContentOptions& web_content_options() { return the().m_web_content_options; }

    static Requests::RequestClient& request_server_client() { return *the().m_request_server_client; }
    static ImageDecoderClient::Client& image_decoder_client() { return *the().m_image_decoder_client; }

    static CookieJar& cookie_jar() { return *the().m_cookie_jar; }

    Core::EventLoop& event_loop() { return m_event_loop; }

    ErrorOr<void> launch_services();

    void add_child_process(Process&&);

    // FIXME: Should these methods be part of Application, instead of deferring to ProcessManager?
#if defined(AK_OS_MACH)
    void set_process_mach_port(pid_t, Core::MachPort&&);
#endif
    Optional<Process&> find_process(pid_t);

    // FIXME: Should we just expose the ProcessManager via a getter?
    void update_process_statistics();
    String generate_process_statistics_html();

    ErrorOr<LexicalPath> path_for_downloaded_file(StringView file) const;

protected:
    template<DerivedFrom<Application> ApplicationType>
    static NonnullOwnPtr<ApplicationType> create(Main::Arguments& arguments, URL::URL new_tab_page_url)
    {
        auto app = adopt_own(*new ApplicationType { {}, arguments });
        app->initialize(arguments, move(new_tab_page_url));

        return app;
    }

    Application();

    virtual void process_did_exit(Process&&);

    virtual void create_platform_arguments(Core::ArgsParser&) { }
    virtual void create_platform_options(ChromeOptions&, WebContentOptions&) { }

    virtual Optional<ByteString> ask_user_for_download_folder() const { return {}; }

private:
    void initialize(Main::Arguments const& arguments, URL::URL new_tab_page_url);

    ErrorOr<void> launch_request_server();
    ErrorOr<void> launch_image_decoder_server();

    static Application* s_the;

    ChromeOptions m_chrome_options;
    WebContentOptions m_web_content_options;

    RefPtr<Requests::RequestClient> m_request_server_client;
    RefPtr<ImageDecoderClient::Client> m_image_decoder_client;

    RefPtr<Database> m_database;
    OwnPtr<CookieJar> m_cookie_jar;

    OwnPtr<Core::TimeZoneWatcher> m_time_zone_watcher;

    Core::EventLoop m_event_loop;
    ProcessManager m_process_manager;
    bool m_in_shutdown { false };
} SWIFT_IMMORTAL_REFERENCE;

}

#define WEB_VIEW_APPLICATION(ApplicationType)                                                           \
public:                                                                                                 \
    static NonnullOwnPtr<ApplicationType> create(Main::Arguments& arguments, URL::URL new_tab_page_url) \
    {                                                                                                   \
        return WebView::Application::create<ApplicationType>(arguments, move(new_tab_page_url));        \
    }                                                                                                   \
                                                                                                        \
    ApplicationType(Badge<WebView::Application>, Main::Arguments&);
