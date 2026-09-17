/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/AudioServerPath.h>

#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>

#if defined(LIBMEDIA_AUDIO_BACKEND_PULSE)
#    include <AK/ScopeGuard.h>
#    include <pulse/pulseaudio.h>
#endif

namespace Audio {

Optional<ByteString> unix_path_from_server_string(StringView server)
{
    server = server.trim_whitespace();

    // A machine identifier may be carried in front of the address.
    if (server.starts_with('{')) {
        auto closing_brace = server.find('}');
        if (!closing_brace.has_value())
            return {};
        server = server.substring_view(*closing_brace + 1);
    }

    if (server.starts_with("unix:"sv))
        server = server.substring_view("unix:"sv.length());

    if (!server.starts_with('/'))
        return {};

    return ByteString { server };
}

#if defined(LIBMEDIA_AUDIO_BACKEND_PULSE)

// Reports the socket the audio library aims at, which it records when the connection is attempted
// and keeps when that attempt fails. A server that is merely not running yet is therefore still
// reported, which matters because the list this feeds is fixed when a renderer starts and cannot be
// changed afterwards.
//
// No main loop runs here. The address is known as soon as the attempt is made, so there is nothing
// to wait for, and waiting would put an unbounded operation on the thread that is about to start a
// renderer.
static Optional<ByteString> ask_audio_library_for_its_server()
{
    auto* main_loop = pa_mainloop_new();
    if (!main_loop)
        return {};

    ScopeGuard free_main_loop = [&] { pa_mainloop_free(main_loop); };

    auto* context = pa_context_new(pa_mainloop_get_api(main_loop), "Ladybird");
    if (!context)
        return {};

    ScopeGuard free_context = [&] {
        pa_context_disconnect(context);
        pa_context_unref(context);
    };

    // Never start a server. We are asking where one is, not asking for one.
    (void)pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);

    auto* server = pa_context_get_server(context);
    if (!server)
        return {};

    // The answer keeps the form it was configured in, so it may carry a transport prefix.
    return unix_path_from_server_string({ server, __builtin_strlen(server) });
}

#endif

Vector<ByteString> audio_server_path_candidates()
{
    Vector<ByteString> candidates;

    // Without the audio library there is no socket to name, and a path built out of this platform's
    // runtime directory would not be one either.
#if defined(LIBMEDIA_AUDIO_BACKEND_PULSE)
    auto add = [&](ByteString path) {
        if (!candidates.contains_slow(path))
            candidates.append(move(path));
    };

    if (auto server = ask_audio_library_for_its_server(); server.has_value())
        add(server.release_value());

    // Whatever the environment names, whether or not anything answered there. A server that is not
    // running when a renderer starts may be running later, and a renderer cannot be given a new
    // allowlist once it has one.
    if (auto server = Core::Environment::get("PULSE_SERVER"sv); server.has_value()) {
        for (auto entry : server->split_view(' ')) {
            if (auto path = unix_path_from_server_string(entry); path.has_value())
                add(path.release_value());
        }
    }

    // The usual socket, in case nothing was listening when we asked.
    if (auto runtime_path = Core::Environment::get("PULSE_RUNTIME_PATH"sv); runtime_path.has_value())
        add(ByteString::formatted("{}/native", *runtime_path));
    if (auto runtime_directory = Core::StandardPaths::runtime_directory(); !runtime_directory.is_error())
        add(ByteString::formatted("{}/pulse/native", runtime_directory.value()));
#endif

    return candidates;
}

}
