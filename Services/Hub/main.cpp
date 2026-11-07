/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <LibIPC/StartupTransport.h>
#include <LibMain/Main.h>

#if defined(AK_OS_MACOS)
#    include <Hub/SpellCheckerMac.h>
#endif

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto application = TRY(Hub::Application::create(arguments, nullptr));
    if (application->should_exit_after_profile_coordination()) {
        outln("Opening in existing process");
        return 0;
    }

#if defined(AK_OS_MACOS)
    application->set_spell_checker(Hub::create_platform_spell_checker());
#endif

    // Headless mode runs on its own, with no UI to serve.
    RefPtr<Hub::ConnectionFromUI> connection;
    if (!Hub::Application::browser_options().headless_mode.has_value()) {
        auto transport = TRY(IPC::take_over_startup_transport(application->ui_mach_server_name()));
        connection = Hub::ConnectionFromUI::construct(move(transport));
    }

    return application->execute();
}
