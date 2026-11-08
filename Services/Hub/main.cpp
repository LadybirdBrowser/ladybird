/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>
#include <LibCore/GeolocationProvider.h>
#include <LibIPC/StartupTransport.h>
#include <LibMain/Main.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto application = TRY(Hub::Application::create(arguments, nullptr));
    if (application->should_exit_after_profile_coordination()) {
        outln("Opening in existing process");
        return 0;
    }

    // Headless mode runs on its own, with no UI to serve.
    RefPtr<Hub::ConnectionFromUI> connection;
    if (!Hub::Application::browser_options().headless_mode.has_value()) {
        auto transport = TRY(IPC::take_over_startup_transport(application->ui_mach_server_name()));
        connection = Hub::ConnectionFromUI::construct(move(transport));

        // NB: Translating and finding the location need a run loop on macOS, and the UI is the process that has one.
        if (application->ui_has_translator())
            application->set_translator(make<Hub::TranslatorInUI>());
        if (auto const* info = application->ui_info(); info && info->has_spell_checker)
            application->set_spell_checker(make<Hub::SpellCheckerInUI>(info->spell_checker_learns_for_every_tag, info->spell_checker_can_suggest));
        Core::GeolocationProvider::set_provider_functions(
            []() -> ErrorOr<NonnullOwnPtr<Core::GeolocationProvider>> { return make<Hub::GeolocationProviderInUI>(); },
            [] { return Hub::Application::the().ui_has_location_source(); });
    }

    return application->execute();
}
