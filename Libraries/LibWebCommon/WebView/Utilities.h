/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

namespace WebView {

// What a WebContent process exits with, plus the signal, if a worker that runs on one of its threads crashes it. This
// tells such a crash from one of the process's own: the tests that crash a WebContent process on purpose have only the
// latter. It is above the 128 plus signal of an ordinary crash handler, so that it still reads as a crash.
constexpr int worker_thread_crash_exit_status = 192;

WEBCOMMON_API void platform_init(Optional<ByteString> ladybird_binary_path = {});
WEBCOMMON_API ErrorOr<Vector<ByteString>> get_paths_for_helper_process(StringView process_name);

WEBCOMMON_API extern ByteString& s_ladybird_resource_root;
WEBCOMMON_API Optional<ByteString const&> mach_server_name();
WEBCOMMON_API void set_mach_server_name(ByteString name);
WEBCOMMON_API ByteString mach_server_name_for_process(StringView process_name, pid_t pid);

WEBCOMMON_API ErrorOr<void> handle_attached_debugger();

WEBCOMMON_API String host_for_settings(URL::URL const&);

}
