/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Application.h"
#include "BrowserWindow.h"
#include "EventLoopImplementationQt.h"
#include "Settings.h"
#include "WebContentView.h"
#include <Ladybird/HelperProcess.h>
#include <Ladybird/Utilities.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/ChromeProcess.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/URL.h>

#if defined(AK_OS_MACOS)
#    include <Ladybird/MachPortServer.h>
#endif

namespace Ladybird {

// FIXME: Find a place to put this declaration (and other helper functions).
bool is_using_dark_system_theme(QWidget&);
bool is_using_dark_system_theme(QWidget& widget)
{
    // FIXME: Qt does not provide any method to query if the system is using a dark theme. We will have to implement
    //        platform-specific methods if we wish to have better detection. For now, this inspects if Qt is using a
    //        dark color for widget backgrounds using Rec. 709 luma coefficients.
    //        https://en.wikipedia.org/wiki/Rec._709#Luma_coefficients

    auto color = widget.palette().color(widget.backgroundRole());
    auto luma = 0.2126f * color.redF() + 0.7152f * color.greenF() + 0.0722f * color.blueF();

    return luma <= 0.5f;
}

}

static ErrorOr<void> handle_attached_debugger()
{
#ifdef AK_OS_LINUX
    // Let's ignore SIGINT if we're being debugged because GDB
    // incorrectly forwards the signal to us even when it's set to
    // "nopass". See https://sourceware.org/bugzilla/show_bug.cgi?id=9425
    // for details.
    if (TRY(Core::Process::is_being_debugged())) {
        dbgln("Debugger is attached, ignoring SIGINT");
        TRY(Core::System::signal(SIGINT, SIG_IGN));
    }
#endif
    return {};
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    Core::EventLoopManager::install(*new Ladybird::EventLoopManagerQt);

    auto app = Ladybird::Application::create(arguments, ak_url_from_qstring(Ladybird::Settings::the()->new_tab_page()));

    static_cast<Ladybird::EventLoopImplementationQt&>(Core::EventLoop::current().impl()).set_main_loop();
    TRY(handle_attached_debugger());

    platform_init();

    WebView::ChromeProcess chrome_process;

    if (app->chrome_options().force_new_process == WebView::ForceNewProcess::No) {
        auto disposition = TRY(chrome_process.connect(app->chrome_options().raw_urls, app->chrome_options().new_window));

        if (disposition == WebView::ChromeProcess::ProcessDisposition::ExitProcess) {
            outln("Opening in existing process");
            return 0;
        }
    }

    chrome_process.on_new_tab = [&](auto const& urls) {
        auto& window = app->active_window();
        for (size_t i = 0; i < urls.size(); ++i) {
            window.new_tab_from_url(urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No);
        }
        window.show();
        window.activateWindow();
        window.raise();
    };

    app->on_open_file = [&](auto file_url) {
        auto& window = app->active_window();
        window.view().load(file_url);
    };

#if defined(AK_OS_MACOS)
    auto mach_port_server = make<Ladybird::MachPortServer>();
    set_mach_server_name(mach_port_server->server_port_name());
    mach_port_server->on_receive_child_mach_port = [&app](auto pid, auto port) {
        app->set_process_mach_port(pid, move(port));
    };
    mach_port_server->on_receive_backing_stores = [](Ladybird::MachPortServer::BackingStoresMessage message) {
        if (auto view = WebView::WebContentClient::view_for_pid_and_page_id(message.pid, message.page_id); view.has_value())
            view->did_allocate_iosurface_backing_stores(message.front_backing_store_id, move(message.front_backing_store_port), message.back_backing_store_id, move(message.back_backing_store_port));
    };
#endif

    copy_default_config_files(Ladybird::Settings::the()->directory());

    // FIXME: Create an abstraction to re-spawn the RequestServer and re-hook up its client hooks to each tab on crash
    auto request_server_paths = TRY(get_paths_for_helper_process("RequestServer"sv));
    auto requests_client = TRY(launch_request_server_process(request_server_paths, s_ladybird_resource_root));
    app->request_server_client = move(requests_client);

    TRY(app->initialize_image_decoder());

    chrome_process.on_new_window = [&](auto const& urls) {
        app->new_window(urls);
    };

    auto& window = app->new_window(app->chrome_options().urls);
    window.setWindowTitle("Ladybird");

    if (Ladybird::Settings::the()->is_maximized()) {
        window.showMaximized();
    } else {
        auto last_position = Ladybird::Settings::the()->last_position();
        if (last_position.has_value())
            window.move(last_position.value());
        window.resize(Ladybird::Settings::the()->last_size());
    }

    window.show();

    return app->execute();
}
