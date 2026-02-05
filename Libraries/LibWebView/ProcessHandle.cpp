/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/ProcessHandle.h>

template<>
ErrorOr<void> IPC::encode(IPC::Encoder& encoder, WebView::ProcessHandle const& handle)
{
    TRY(encoder.encode(handle.pid));
    return {};
}

template<>
ErrorOr<WebView::ProcessHandle> IPC::decode(IPC::Decoder& decoder)
{
    auto pid = TRY(decoder.decode<pid_t>());
    return WebView::ProcessHandle { pid };
}
