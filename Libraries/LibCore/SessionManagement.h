/*
 * Copyright (c) 2022, Peter Elliott <pelliott@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Types.h>

namespace Core::SessionManagement {

ErrorOr<pid_t> root_session_id(Optional<pid_t> force_sid = {});
ErrorOr<void> logout(Optional<pid_t> force_sid = {});

ErrorOr<ByteString> parse_path_with_sid(StringView general_path, Optional<pid_t> force_sid = {});
ErrorOr<void> create_session_temporary_directory_if_needed(uid_t uid, gid_t gid, Optional<pid_t> force_sid = {});

}
