/*
 * Copyright (c) 2022, Peter Elliott <pelliott@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Directory.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/System.h>

namespace Core::SessionManagement {

ErrorOr<pid_t> root_session_id([[maybe_unused]] Optional<pid_t> force_sid)
{
    return 0;
}

ErrorOr<void> logout(Optional<pid_t> force_sid)
{
    pid_t sid = TRY(root_session_id(force_sid));
    TRY(System::kill(-sid, SIGTERM));
    return {};
}

ErrorOr<ByteString> parse_path_with_sid(StringView general_path, Optional<pid_t> force_sid)
{
    if (general_path.contains("%sid"sv)) {
        pid_t sid = TRY(root_session_id(force_sid));
        return general_path.replace("%sid"sv, ByteString::number(sid), ReplaceMode::All);
    }
    return ByteString(general_path);
}

ErrorOr<void> create_session_temporary_directory_if_needed(uid_t uid, gid_t gid, Optional<pid_t> force_sid)
{
    pid_t sid = TRY(root_session_id(force_sid));
    auto const temporary_directory = ByteString::formatted("/tmp/session/{}", sid);
    auto directory = TRY(Core::Directory::create(temporary_directory, Core::Directory::CreateDirectories::Yes));
    TRY(directory.chown(uid, gid));
    return {};
}

}
