/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/LexicalPath.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibWebView/CameraDaemonSocket.h>
#include <sys/stat.h>

namespace WebView {

// PIPEWIRE_REMOTE names one remote, or several as a JSON array that are tried in turn. PipeWire's JSON is relaxed: its
// strings need no quotes, and the values need no commas between them.
static Vector<ByteString> remote_names_from(StringView remote)
{
    auto trimmed = remote.trim_whitespace();
    if (!trimmed.starts_with('[') || !trimmed.ends_with(']'))
        return { ByteString { remote } };

    Vector<ByteString> names;
    auto inner = trimmed.substring_view(1, trimmed.length() - 2);
    size_t index = 0;
    while (index < inner.length()) {
        auto character = inner[index];
        if (is_ascii_space(character) || character == ',') {
            ++index;
            continue;
        }
        size_t start = index;
        size_t end = index;
        if (character == '"') {
            start = ++index;
            while (index < inner.length() && inner[index] != '"')
                index += inner[index] == '\\' ? 2 : 1;
            end = min(index, inner.length());
            ++index;
        } else {
            while (index < inner.length() && !is_ascii_space(inner[index]) && inner[index] != ',')
                ++index;
            end = index;
        }
        if (end > start)
            names.append(inner.substring_view(start, end - start));
    }
    return names;
}

static bool is_socket(ByteString const& path)
{
    auto status = Core::System::stat(path);
    return !status.is_error() && S_ISSOCK(status.value().st_mode);
}

ErrorOr<ByteString> camera_daemon_socket_path()
{
    auto remote = Core::Environment::get("PIPEWIRE_REMOTE"sv).value_or({});
    auto names = remote_names_from(remote.is_empty() ? "pipewire-0"sv : remote);

    // A name is looked for in PipeWire's runtime directory, which is the user's unless PIPEWIRE_RUNTIME_DIR says
    // otherwise, and then in the system's. A name that is a path says where the socket is outright.
    auto runtime_directory = Core::Environment::get("PIPEWIRE_RUNTIME_DIR"sv);
    if (!runtime_directory.has_value() || runtime_directory->is_empty())
        runtime_directory = Core::Environment::get("XDG_RUNTIME_DIR"sv);
    static constexpr auto SYSTEM_DIRECTORY = "/run/pipewire"sv;

    Vector<ByteString> candidates;
    for (auto const& name : names) {
        if (name.starts_with('/')) {
            candidates.append(name);
            continue;
        }
        if (runtime_directory.has_value() && !runtime_directory->is_empty())
            candidates.append(LexicalPath::join(*runtime_directory, name).string());
        candidates.append(LexicalPath::join(SYSTEM_DIRECTORY, name).string());
    }
    if (candidates.is_empty())
        return Error::from_string_literal("PIPEWIRE_REMOTE names no remote");

    // NB: The broker uses the first existing socket. Unlike PipeWire, it cannot try another if connecting fails,
    //     including when a dead daemon left a stale socket.
    for (auto const& candidate : candidates) {
        if (is_socket(candidate))
            return candidate;
    }
    return candidates.first();
}

}
