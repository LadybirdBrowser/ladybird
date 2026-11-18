/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>
#include <LibCore/GeolocationProvider.h>
#include <LibCore/Process.h>
#include <LibIPC/StartupTransport.h>
#include <LibMain/Main.h>
#include <LibThreading/Thread.h>
#include <LibWebView/BrowserProcess.h>

#if defined(AK_OS_MACOS) || defined(AK_OS_BSD_GENERIC)
#    include <sys/event.h>
#elif defined(AK_OS_LINUX)
#    include <poll.h>
#    include <sys/syscall.h>
#endif
#if !defined(AK_OS_WINDOWS)
#    include <errno.h>
#    include <signal.h>
#    include <unistd.h>
#endif

#if !defined(AK_OS_WINDOWS)
// How long the Hub may take to end by itself once the UI is gone.
static constexpr u32 seconds_to_outlive_the_ui = 10;

static void wait_for_process_to_exit(pid_t pid)
{
#    if defined(AK_OS_MACOS) || defined(AK_OS_BSD_GENERIC)
    auto queue = kqueue();
    if (queue >= 0) {
        struct kevent change {};
        EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        struct kevent event {};
        for (;;) {
            auto count = kevent(queue, &change, 1, &event, 1, nullptr);
            // NB: A process that is gone already cannot be watched. That, like any other failure to watch, is
            //     reported as an event when there is room for one.
            auto failed_to_watch = count > 0 && (event.flags & EV_ERROR) != 0;
            auto error = failed_to_watch ? static_cast<int>(event.data) : errno;
            if ((count > 0 && !failed_to_watch) || ((count < 0 || failed_to_watch) && error == ESRCH)) {
                (void)close(queue);
                return;
            }
            if (failed_to_watch || (count < 0 && error != EINTR))
                break;
        }
        (void)close(queue);
    }
#    elif defined(AK_OS_LINUX)
    auto process_fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    if (process_fd >= 0) {
        // NB: The descriptor of a process becomes readable once the process has exited.
        pollfd poll_fd { .fd = process_fd, .events = POLLIN, .revents = 0 };
        while (poll(&poll_fd, 1, -1) < 0 && errno == EINTR)
            ;
        (void)close(process_fd);
        return;
    }
    if (errno == ESRCH)
        return;
#    endif

    // NB: Without a way to be told of the exit, look for the process now and then.
    while (kill(pid, 0) == 0 || errno != ESRCH)
        sleep(1);
}
#endif

// The Hub ends when the UI does, which it learns from its connection to the UI. It does not learn that while it has
// not taken that connection yet, or while its main thread waits for something else. A Hub that the UI left behind has
// no window to tell the user of it, yet it keeps its helpers running and the profile to itself. So this watches the UI
// from a thread of its own, and ends a Hub that does not end by itself.
static void end_with_ui_process([[maybe_unused]] Main::Arguments const& arguments)
{
    // FIXME: Do this on Windows as well.
#if !defined(AK_OS_WINDOWS)
    // NB: This starts before the arguments are parsed, as that is where the Hub may begin to wait.
    Optional<pid_t> ui_pid;
    for (auto argument : arguments.strings) {
        if (argument.starts_with("--ui-pid="sv))
            ui_pid = argument.substring_view("--ui-pid="sv.length()).to_number<pid_t>();
    }
    if (!ui_pid.has_value() || *ui_pid <= 0)
        return;

    auto watcher = Threading::Thread::construct("UIExitWatcher"sv, [pid = *ui_pid]() -> intptr_t {
        wait_for_process_to_exit(pid);
        sleep(seconds_to_outlive_the_ui);
        // NB: Nothing is logged here, as a Hub that is stuck writing to its output would keep us from ending it.
        Core::Process::terminate_immediately(1);
    });
    watcher->start();
    watcher->detach();
#endif
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    end_with_ui_process(arguments);

    auto application = TRY(Hub::Application::create(arguments, nullptr));
    if (application->browser_is_unresponsive()) {
        warnln("Ladybird is already running with this profile, but it is not responding");
        return WebView::BrowserProcess::unresponsive_browser_exit_status;
    }
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
