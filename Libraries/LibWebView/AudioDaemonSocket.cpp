/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/AudioDaemonSocket.h>

#if defined(HAVE_PULSEAUDIO)
#    include <AK/ScopeGuard.h>
#    include <AK/Time.h>
#    include <pulse/pulseaudio.h>
#    include <string.h>
#endif

namespace WebView {

Optional<ByteString> unix_path_from_server_string(StringView server)
{
    server = server.trim_whitespace();

    // A machine identifier may be carried in front of the address.
    if (server.starts_with('{')) {
        auto closing_brace = server.find('}');
        if (!closing_brace.has_value())
            return {};
        server = server.substring_view(closing_brace.value() + 1);
    }

    if (server.starts_with("unix:"sv))
        server = server.substring_view("unix:"sv.length());

    if (!server.starts_with('/'))
        return {};

    return ByteString { server };
}

#if defined(HAVE_PULSEAUDIO)

ErrorOr<ByteString> audio_daemon_socket_path()
{
    auto* main_loop = pa_mainloop_new();
    if (!main_loop)
        return Error::from_string_literal("Unable to create a PulseAudio main loop");
    ScopeGuard free_main_loop = [&] { pa_mainloop_free(main_loop); };

    auto* context = pa_context_new(pa_mainloop_get_api(main_loop), "Ladybird");
    if (!context)
        return Error::from_string_literal("Unable to create a PulseAudio context");
    ScopeGuard free_context = [&] {
        pa_context_disconnect(context);
        pa_context_unref(context);
    };

    // Only the attempt that succeeds says which of the library's candidates is the daemon, so this runs the
    // connection to its end.
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        warnln("Unable to look for the audio daemon: {}", pa_strerror(pa_context_errno(context)));
        return Error::from_string_literal("Unable to look for the audio daemon");
    }

    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(4);
    for (;;) {
        switch (pa_context_get_state(context)) {
        case PA_CONTEXT_READY: {
            auto const* server = pa_context_get_server(context);
            if (!server)
                return Error::from_string_literal("The audio daemon did not say where it was reached");
            auto path = unix_path_from_server_string({ server, strlen(server) });
            if (!path.has_value())
                return Error::from_string_literal("The audio daemon is not reachable over a UNIX socket");
            return path.release_value();
        }
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            warnln("The audio daemon could not be reached: {}", pa_strerror(pa_context_errno(context)));
            return Error::from_string_literal("The audio daemon could not be reached");
        default:
            break;
        }

        auto remaining = (deadline - MonotonicTime::now()).to_microseconds();
        if (remaining <= 0)
            return Error::from_string_literal("The audio daemon did not answer in time");
        if (pa_mainloop_prepare(main_loop, static_cast<int>(remaining)) < 0 || pa_mainloop_poll(main_loop) < 0 || pa_mainloop_dispatch(main_loop) < 0)
            return Error::from_string_literal("The PulseAudio main loop failed");
    }
}

#else

ErrorOr<ByteString> audio_daemon_socket_path()
{
    return Error::from_string_literal("No audio daemon is known on this platform");
}

#endif

}
