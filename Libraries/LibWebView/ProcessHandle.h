/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibIPC/Forward.h>

namespace WebView {

struct ProcessHandle {
    // FIXME: Use mach_port_t on macOS/Hurd and HANDLE on Windows.
    pid_t pid { -1 };
};

}

template<>
ErrorOr<void> IPC::encode(IPC::Encoder&, WebView::ProcessHandle const&);

template<>
ErrorOr<WebView::ProcessHandle> IPC::decode(IPC::Decoder&);
