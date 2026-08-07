/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibThreading/ThreadPool.h>
#include <UI/Qt/ExternalURLActivationToken.h>
#include <UI/Qt/ExternalURLHandler.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

namespace Ladybird {

static constexpr auto default_ladybird_desktop_id = "org.ladybird.Ladybird.desktop"sv;
static Core::EventLoop* s_main_thread_event_loop;

struct LadybirdAppLaunchContext {
    GAppLaunchContext parent_instance;
    Optional<String> activation_token;
};

struct LadybirdAppLaunchContextClass {
    GAppLaunchContextClass parent_class;
};

GType ladybird_app_launch_context_get_type();

G_DEFINE_TYPE(LadybirdAppLaunchContext, ladybird_app_launch_context, G_TYPE_APP_LAUNCH_CONTEXT)

static char* copy_activation_token(String const& activation_token)
{
    auto bytes = activation_token.bytes_as_string_view();
    return g_strndup(bytes.characters_without_null_termination(), bytes.length());
}

static char* ladybird_app_launch_context_get_startup_notify_id(GAppLaunchContext* context, GAppInfo*, GList*)
{
    auto* self = reinterpret_cast<LadybirdAppLaunchContext*>(context);
    if (!self->activation_token.has_value())
        return nullptr;
    return copy_activation_token(*self->activation_token);
}

static void ladybird_app_launch_context_finalize(GObject* object)
{
    auto* self = reinterpret_cast<LadybirdAppLaunchContext*>(object);
    self->activation_token.~Optional<String>();
    G_OBJECT_CLASS(ladybird_app_launch_context_parent_class)->finalize(object);
}

static void ladybird_app_launch_context_class_init(LadybirdAppLaunchContextClass* klass)
{
    auto* launch_context_class = G_APP_LAUNCH_CONTEXT_CLASS(klass);
    auto* object_class = G_OBJECT_CLASS(klass);
    launch_context_class->get_startup_notify_id = ladybird_app_launch_context_get_startup_notify_id;
    object_class->finalize = ladybird_app_launch_context_finalize;
}

static void ladybird_app_launch_context_init(LadybirdAppLaunchContext* self)
{
    new (&self->activation_token) Optional<String> {};
}

static GAppLaunchContext* create_launch_context(Optional<String> const& activation_token)
{
    if (!activation_token.has_value() || activation_token->is_empty())
        return nullptr;

    auto* context = reinterpret_cast<LadybirdAppLaunchContext*>(g_object_new(ladybird_app_launch_context_get_type(), nullptr));
    context->activation_token = *activation_token;

    // GLib versions before 2.76 do not propagate the startup notification ID as an XDG activation token.
    if (glib_check_version(2, 76, 0)) {
        auto* token = copy_activation_token(*context->activation_token);
        g_app_launch_context_setenv(G_APP_LAUNCH_CONTEXT(context), "XDG_ACTIVATION_TOKEN", token);
        g_free(token);
    }

    return G_APP_LAUNCH_CONTEXT(context);
}

static Optional<ByteString> current_process_desktop_file()
{
    auto desktop_file = Core::Environment::get("GIO_LAUNCHED_DESKTOP_FILE"sv);
    auto desktop_file_pid = Core::Environment::get("GIO_LAUNCHED_DESKTOP_FILE_PID"sv);
    if (!desktop_file.has_value() || !desktop_file_pid.has_value())
        return {};

    auto pid = desktop_file_pid->to_number<pid_t>();
    if (!pid.has_value() || *pid != Core::System::getpid())
        return {};
    return ByteString { desktop_file.value() };
}

static bool is_ladybird(GAppInfo* app_info, StringView ladybird_desktop_id, Optional<ByteString> const& ladybird_desktop_file)
{
    auto* application_id = g_app_info_get_id(app_info);
    if (application_id && application_id == ladybird_desktop_id)
        return true;

    if (!ladybird_desktop_file.has_value())
        return false;

    auto* launched_app_info = g_desktop_app_info_new_from_filename(ladybird_desktop_file->characters());
    if (!launched_app_info)
        return false;
    ScopeGuard release_app_info = [&] {
        g_object_unref(launched_app_info);
    };
    return g_app_info_equal(app_info, G_APP_INFO(launched_app_info));
}

static bool is_dispatcher(GAppInfo* app_info)
{
    static constexpr Array dispatchers {
        "exo-open"sv,
        "gio"sv,
        "gnome-open"sv,
        "gvfs-open"sv,
        "kde-open"sv,
        "kde-open5"sv,
        "kde-open6"sv,
        "kioclient"sv,
        "kioclient5"sv,
        "kioclient6"sv,
        "xdg-open"sv,
    };

    auto* executable = g_app_info_get_executable(app_info);
    if (!executable)
        return false;
    return dispatchers.contains_slow(LexicalPath::basename(executable));
}

class GIOExternalURLHandler final : public WebView::ExternalURLHandler {
public:
    GIOExternalURLHandler(GAppInfo* app_info, String application_name, bool is_ladybird)
        : WebView::ExternalURLHandler(move(application_name), is_ladybird)
        , m_app_info(app_info)
    {
    }

    virtual ~GIOExternalURLHandler() override
    {
        g_object_unref(m_app_info);
    }

    virtual void launch(URL::URL const& url, LaunchCallback callback) const override
    {
        NonnullRefPtr protected_this { *this };
        request_external_url_activation_token([protected_this = move(protected_this), url, callback = move(callback)](Optional<String> activation_token) mutable {
            callback(protected_this->launch(url, activation_token));
        });
    }

private:
    bool launch(URL::URL const& url, Optional<String> const& activation_token) const
    {
        auto serialized_url = url.serialize().to_byte_string();
        GList uris {
            .data = const_cast<char*>(serialized_url.characters()),
            .next = nullptr,
            .prev = nullptr,
        };
        auto* launch_context = create_launch_context(activation_token);
        ScopeGuard release_launch_context = [&] {
            if (launch_context)
                g_object_unref(launch_context);
        };
        GError* error = nullptr;
        if (g_app_info_launch_uris(m_app_info, &uris, launch_context, &error))
            return true;

        ScopeGuard release_error = [&] {
            if (error)
                g_error_free(error);
        };
        if (error) {
            warnln("Unable to launch external URL with {}: {}", application_name(), error->message);
        } else {
            warnln("Unable to launch external URL with {}", application_name());
        }
        return false;
    }

    GAppInfo* m_app_info { nullptr };
};

void initialize_external_url_handler(Core::EventLoop& main_thread_event_loop)
{
    VERIFY(!s_main_thread_event_loop);
    s_main_thread_event_loop = &main_thread_event_loop;
}

void resolve_external_url_handler(URL::URL const& url, String ladybird_desktop_id, WebView::ExternalURLHandlerCallback callback)
{
    VERIFY(s_main_thread_event_loop);

    auto scheme = url.scheme().to_byte_string();
    if (ladybird_desktop_id.is_empty())
        ladybird_desktop_id = MUST(String::from_utf8(default_ladybird_desktop_id));
    else if (!ladybird_desktop_id.bytes_as_string_view().ends_with(".desktop"sv))
        ladybird_desktop_id = MUST(String::formatted("{}.desktop", ladybird_desktop_id));
    auto ladybird_desktop_file = current_process_desktop_file();
    Threading::ThreadPool::the().submit([scheme = move(scheme), ladybird_desktop_id = move(ladybird_desktop_id), ladybird_desktop_file = move(ladybird_desktop_file), callback = move(callback)]() mutable {
        RefPtr<WebView::ExternalURLHandler> handler;
        if (auto* app_info = g_app_info_get_default_for_uri_scheme(scheme.characters())) {
            if (is_dispatcher(app_info)) {
                g_object_unref(app_info);
            } else {
                auto app_is_ladybird = is_ladybird(app_info, ladybird_desktop_id, ladybird_desktop_file);

                auto* application_name = g_app_info_get_display_name(app_info);
                auto name = application_name
                    ? String::from_utf8_with_replacement_character({ application_name, strlen(application_name) })
                    : "another application"_string;

                handler = adopt_ref(*new GIOExternalURLHandler(app_info, move(name), app_is_ladybird));
            }
        }

        s_main_thread_event_loop->deferred_invoke([callback = move(callback), handler = move(handler)]() mutable {
            callback(move(handler));
        });
    });
}

}
