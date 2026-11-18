/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/AtomicRefCounted.h>
#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibFileSystem/FileSystem.h>
#include <LibThreading/ThreadPool.h>
#include <UI/Qt/DefaultBrowserLinux.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <sys/wait.h>

namespace Ladybird {

using WebView::DefaultBrowserStatus;

namespace Detail {

// The desktop is asked on a worker thread, which may finish after the browser has quit. Only this atomically counted
// link crosses threads, and only the main thread follows it.
struct DefaultBrowserLinuxLink : public AtomicRefCounted<DefaultBrowserLinuxLink> {
    DefaultBrowserLinux* default_browser { nullptr };
};

}

// What a desktop has a web browser open: links, and the web pages kept in files.
static constexpr Array browser_content_types {
    "x-scheme-handler/http",
    "x-scheme-handler/https",
    "text/html",
    "application/xhtml+xml",
};

static bool opens_uris_with_scheme(GAppInfo* ladybird, char const* scheme)
{
    auto* handler = g_app_info_get_default_for_uri_scheme(scheme);
    if (!handler)
        return false;
    ScopeGuard release_handler = [&] {
        g_object_unref(handler);
    };

    return g_app_info_equal(handler, ladybird);
}

struct XdgSettingsResult {
    bool succeeded { false };
    String standard_output;
    String standard_error;
};

// Empty if xdg-settings could not be run, as when it is not installed.
static Optional<XdgSettingsResult> run_xdg_settings(char const* operation, Optional<ByteString const&> desktop_id = {})
{
    Vector<char const*, 5> arguments { "xdg-settings", operation, "default-web-browser" };
    if (desktop_id.has_value())
        arguments.append(desktop_id->characters());
    arguments.append(nullptr);

    gchar* standard_output = nullptr;
    gchar* standard_error = nullptr;
    gint wait_status = 0;
    if (!g_spawn_sync(nullptr, const_cast<gchar**>(arguments.data()), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, &standard_output, &standard_error, &wait_status, nullptr))
        return {};
    ScopeGuard free_output = [&] {
        g_free(standard_output);
        g_free(standard_error);
    };

    auto trimmed = [](char const* text) {
        return String::from_utf8_with_replacement_character(StringView { text, strlen(text) }.trim_whitespace());
    };

    return XdgSettingsResult {
        .succeeded = WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0,
        .standard_output = trimmed(standard_output),
        .standard_error = trimmed(standard_error),
    };
}

// Empty if xdg-settings is missing or gives no answer, as on a desktop it does not know.
static Optional<bool> xdg_settings_calls_default(ByteString const& desktop_id)
{
    auto result = run_xdg_settings("check", desktop_id);
    if (!result.has_value() || !result->succeeded)
        return {};

    if (result->standard_output == "yes"sv)
        return true;
    if (result->standard_output == "no"sv)
        return false;
    return {};
}

static bool xdg_settings_names_as_browser(ByteString const& desktop_id)
{
    auto result = run_xdg_settings("get");
    return result.has_value() && result->succeeded && result->standard_output == desktop_id.view();
}

DefaultBrowserLinux::DefaultBrowserLinux(ByteString desktop_id)
    : m_desktop_id(move(desktop_id))
    , m_link(adopt_ref(*new Detail::DefaultBrowserLinuxLink))
{
    m_link->default_browser = this;
}

DefaultBrowserLinux::~DefaultBrowserLinux()
{
    m_link->default_browser = nullptr;
}

void DefaultBrowserLinux::refresh()
{
    m_refresh_wanted = true;
    ask_desktop_if_idle();
}

void DefaultBrowserLinux::make_default(Function<void(WebView::MakeDefaultBrowserResult const&)> on_complete)
{
    // Requests made while the desktop is making Ladybird the default share its answer.
    m_pending_requests.append(move(on_complete));
    ask_desktop_if_idle();
}

// The desktop is asked one thing at a time, so that answers arrive in the order their statuses were read.
void DefaultBrowserLinux::ask_desktop_if_idle()
{
    if (m_asking_desktop)
        return;

    auto make_default = m_pending_requests.is_empty() ? MakeDefault::No : MakeDefault::Yes;
    if (make_default == MakeDefault::No && !m_refresh_wanted)
        return;

    // Making Ladybird the default ends by reading the status, which is all a refresh does.
    m_refresh_wanted = false;
    m_asking_desktop = true;

    Threading::ThreadPool::the().submit([link = m_link, desktop_id = m_desktop_id, make_default, event_loop = Core::EventLoop::current_weak()] {
        auto answer = ask_desktop(desktop_id, make_default);

        auto strong_event_loop = event_loop->take();
        if (!strong_event_loop)
            return;
        strong_event_loop->deferred_invoke([link, make_default, answer = move(answer)] {
            if (link->default_browser)
                link->default_browser->desktop_did_answer(make_default, answer);
        });
    });
}

void DefaultBrowserLinux::desktop_did_answer(MakeDefault make_default, DesktopAnswer const& answer)
{
    did_read_status(answer.status);

    if (make_default == MakeDefault::Yes) {
        // The desktop may keep opening links elsewhere without reporting an error, which is a failure all the same.
        WebView::MakeDefaultBrowserResult result;
        if (answer.status != DefaultBrowserStatus::Default) {
            result.failed = true;
            result.system_message = answer.error_message;
        }

        auto pending_requests = move(m_pending_requests);
        for (auto& on_complete : pending_requests)
            on_complete(result);
    }

    m_asking_desktop = false;
    ask_desktop_if_idle();
}

DefaultBrowserLinux::DesktopAnswer DefaultBrowserLinux::ask_desktop(ByteString const& desktop_id, MakeDefault make_default)
{
    // A Flatpak is shown its own applications and defaults, never the desktop's.
    if (FileSystem::exists("/.flatpak-info"sv))
        return {};

    // The desktop opens links in applications it has a desktop file for, which a browser run from a build directory
    // or an unpacked archive does not have.
    auto* desktop_app_info = g_desktop_app_info_new(desktop_id.characters());
    if (!desktop_app_info)
        return {};
    ScopeGuard release_desktop_app_info = [&] {
        g_object_unref(desktop_app_info);
    };
    auto* ladybird = G_APP_INFO(desktop_app_info);

    DesktopAnswer answer;
    if (make_default == MakeDefault::Yes) {
        for (auto const* content_type : browser_content_types) {
            GError* error = nullptr;
            if (g_app_info_set_as_default_for_type(ladybird, content_type, &error) || !error)
                continue;

            auto message = String::from_utf8_with_replacement_character({ error->message, strlen(error->message) });
            g_clear_error(&error);

            warnln("Unable to make Ladybird the default application for {}: {}", content_type, message);
            if (!answer.error_message.has_value())
                answer.error_message = move(message);
        }

        // A desktop such as Xfce opens links with a browser setting of its own, which xdg-settings can change and GIO
        // cannot. xdg-settings goes last, as it may point GIO's schemes at a launcher of the desktop's that follows
        // that setting.
        auto result = run_xdg_settings("set", desktop_id);
        if (result.has_value() && !result->succeeded) {
            warnln("Unable to make Ladybird the default browser with xdg-settings: {}", result->standard_error);
            if (!answer.error_message.has_value() && !result->standard_error.is_empty())
                answer.error_message = move(result->standard_error);
        }
    }

    auto opens_links = opens_uris_with_scheme(ladybird, "http") && opens_uris_with_scheme(ladybird, "https");

    // On a desktop such as Xfce, xdg-open follows a browser setting that xdg-settings reads and GIO does not. GIO
    // answers where xdg-settings has no answer.
    auto is_default = xdg_settings_calls_default(desktop_id).value_or(opens_links);

    // xdg-settings also says no where links open in Ladybird and HTML files do not, as after choosing the browser in
    // Plasma's settings. The browser it names tells that apart from a desktop's own setting holding another browser.
    if (!is_default && opens_links)
        is_default = xdg_settings_names_as_browser(desktop_id);

    answer.status = is_default ? DefaultBrowserStatus::Default : DefaultBrowserStatus::NotDefault;
    return answer;
}

}
