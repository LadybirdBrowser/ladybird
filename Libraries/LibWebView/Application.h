/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/Swift.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Forward.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/Forward.h>
#include <LibImageDecoderClient/Client.h>
#include <LibMain/Main.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/URL.h>
#include <LibWebView/Options.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Settings.h>
#include <LibWebView/StorageJar.h>

namespace WebView {

struct ApplicationSettingsObserver;

class Application : public DevTools::DevToolsDelegate {
    AK_MAKE_NONCOPYABLE(Application);

public:
    virtual ~Application();

    ErrorOr<int> execute();

    static Application& the() { return *s_the; }

    static Settings& settings() { return the().m_settings; }

    static BrowserOptions const& browser_options() { return the().m_browser_options; }
    static WebContentOptions& web_content_options() { return the().m_web_content_options; }

    static Requests::RequestClient& request_server_client() { return *the().m_request_server_client; }
    static ImageDecoderClient::Client& image_decoder_client() { return *the().m_image_decoder_client; }

    static CookieJar& cookie_jar() { return *the().m_cookie_jar; }
    static StorageJar& storage_jar() { return *the().m_storage_jar; }

    static ProcessManager& process_manager() { return *the().m_process_manager; }

    ErrorOr<NonnullRefPtr<WebContentClient>> launch_web_content_process(ViewImplementation&);

    void add_child_process(Process&&);

    // FIXME: Should these methods be part of Application, instead of deferring to ProcessManager?
#if defined(AK_OS_MACH)
    void set_process_mach_port(pid_t, Core::MachPort&&);
#endif
    Optional<Process&> find_process(pid_t);

    ErrorOr<LexicalPath> path_for_downloaded_file(StringView file) const;

    enum class DevtoolsState {
        Disabled,
        Enabled,
    };
    ErrorOr<DevtoolsState> toggle_devtools_enabled();
    void refresh_tab_list();

protected:
    explicit Application(Optional<ByteString> ladybird_binary_path = {});

    ErrorOr<void> initialize(Main::Arguments const&);

    virtual void process_did_exit(Process&&);

    virtual void create_platform_arguments(Core::ArgsParser&) { }
    virtual void create_platform_options(BrowserOptions&, WebContentOptions&) { }
    virtual NonnullOwnPtr<Core::EventLoop> create_platform_event_loop();

    virtual Optional<ByteString> ask_user_for_download_folder() const { return {}; }

    Main::Arguments& arguments() { return m_arguments; }

private:
    ErrorOr<void> launch_services();
    void launch_spare_web_content_process();
    ErrorOr<void> launch_request_server();
    ErrorOr<void> launch_image_decoder_server();
    ErrorOr<void> launch_devtools_server();

    virtual Vector<DevTools::TabDescription> tab_list() const override;
    virtual Vector<DevTools::CSSProperty> css_property_list() const override;
    virtual void inspect_tab(DevTools::TabDescription const&, OnTabInspectionComplete) const override;
    virtual void listen_for_dom_properties(DevTools::TabDescription const&, OnDOMNodePropertiesReceived) const override;
    virtual void stop_listening_for_dom_properties(DevTools::TabDescription const&) const override;
    virtual void inspect_dom_node(DevTools::TabDescription const&, DOMNodeProperties::Type, Web::UniqueNodeID, Optional<Web::CSS::PseudoElement>) const override;
    virtual void clear_inspected_dom_node(DevTools::TabDescription const&) const override;
    virtual void highlight_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID, Optional<Web::CSS::PseudoElement>) const override;
    virtual void clear_highlighted_dom_node(DevTools::TabDescription const&) const override;
    virtual void listen_for_dom_mutations(DevTools::TabDescription const&, OnDOMMutationReceived) const override;
    virtual void stop_listening_for_dom_mutations(DevTools::TabDescription const&) const override;
    virtual void get_dom_node_inner_html(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeHTMLReceived) const override;
    virtual void get_dom_node_outer_html(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeHTMLReceived) const override;
    virtual void set_dom_node_outer_html(DevTools::TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const override;
    virtual void set_dom_node_text(DevTools::TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const override;
    virtual void set_dom_node_tag(DevTools::TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const override;
    virtual void add_dom_node_attributes(DevTools::TabDescription const&, Web::UniqueNodeID, ReadonlySpan<Attribute>, OnDOMNodeEditComplete) const override;
    virtual void replace_dom_node_attribute(DevTools::TabDescription const&, Web::UniqueNodeID, String const&, ReadonlySpan<Attribute>, OnDOMNodeEditComplete) const override;
    virtual void create_child_element(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const override;
    virtual void insert_dom_node_before(DevTools::TabDescription const&, Web::UniqueNodeID, Web::UniqueNodeID, Optional<Web::UniqueNodeID>, OnDOMNodeEditComplete) const override;
    virtual void clone_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const override;
    virtual void remove_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const override;
    virtual void retrieve_style_sheets(DevTools::TabDescription const&, OnStyleSheetsReceived) const override;
    virtual void retrieve_style_sheet_source(DevTools::TabDescription const&, Web::CSS::StyleSheetIdentifier const&) const override;
    virtual void listen_for_style_sheet_sources(DevTools::TabDescription const&, OnStyleSheetSourceReceived) const override;
    virtual void stop_listening_for_style_sheet_sources(DevTools::TabDescription const&) const override;
    virtual void evaluate_javascript(DevTools::TabDescription const&, String const&, OnScriptEvaluationComplete) const override;
    virtual void listen_for_console_messages(DevTools::TabDescription const&, OnConsoleMessageAvailable, OnReceivedConsoleMessages) const override;
    virtual void stop_listening_for_console_messages(DevTools::TabDescription const&) const override;
    virtual void request_console_messages(DevTools::TabDescription const&, i32) const override;

    static Application* s_the;

    Settings m_settings;
    OwnPtr<ApplicationSettingsObserver> m_settings_observer;

    Main::Arguments m_arguments;
    BrowserOptions m_browser_options;
    WebContentOptions m_web_content_options;

    RefPtr<Requests::RequestClient> m_request_server_client;
    RefPtr<ImageDecoderClient::Client> m_image_decoder_client;

    RefPtr<WebContentClient> m_spare_web_content_process;
    bool m_has_queued_task_to_launch_spare_web_content_process { false };

    RefPtr<Database> m_database;
    OwnPtr<CookieJar> m_cookie_jar;
    OwnPtr<StorageJar> m_storage_jar;

    OwnPtr<Core::TimeZoneWatcher> m_time_zone_watcher;

    OwnPtr<Core::EventLoop> m_event_loop;
    OwnPtr<ProcessManager> m_process_manager;
    bool m_in_shutdown { false };

#if defined(AK_OS_MACOS)
    OwnPtr<MachPortServer> m_mach_port_server;
#endif

    OwnPtr<DevTools::DevToolsServer> m_devtools;
} SWIFT_IMMORTAL_REFERENCE;

}

#define WEB_VIEW_APPLICATION(ApplicationType)                                                                                                \
public:                                                                                                                                      \
    template<typename... ApplicationArguments>                                                                                               \
    static ErrorOr<NonnullOwnPtr<ApplicationType>> create(Main::Arguments const& arguments, ApplicationArguments&&... application_arguments) \
    {                                                                                                                                        \
        auto app = adopt_own(*new ApplicationType { forward<ApplicationArguments>(application_arguments)... });                              \
        TRY(app->initialize(arguments));                                                                                                     \
        return app;                                                                                                                          \
    }                                                                                                                                        \
                                                                                                                                             \
    static ApplicationType& the()                                                                                                            \
    {                                                                                                                                        \
        return static_cast<ApplicationType&>(WebView::Application::the());                                                                   \
    }                                                                                                                                        \
                                                                                                                                             \
private:
