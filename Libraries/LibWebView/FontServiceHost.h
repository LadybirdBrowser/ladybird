/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ConditionVariable.h>
#include <AK/Error.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/Forward.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class FontServerConnection;

// Every connection to the font service, answered on one thread of its own.
//
// The UI process makes synchronous calls into the Compositor and into each renderer, so a font
// question one of them asks while the UI is waiting on it would deadlock if the UI's main thread
// had to answer it. This thread answers font questions and never asks a client anything, so it
// cannot be part of such a cycle.
class WEBVIEW_API FontServiceHost final {
    AK_MAKE_NONCOPYABLE(FontServiceHost);
    AK_MAKE_NONMOVABLE(FontServiceHost);

public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<FontServiceHost> create(FontService&);
    ~FontServiceHost();

    // Opens a connection and returns the end the asking process takes. The connection lives until
    // that process closes it.
    ErrorOr<IPC::TransportHandle> connect();

private:
    friend class FontServerConnection;

    explicit FontServiceHost(FontService&);
    intptr_t thread_main();
    ErrorOr<IPC::TransportHandle> open_connection();

    NonnullRefPtr<FontService> m_font_service;
    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    RefPtr<Core::WeakEventLoopReference> m_event_loop;

    // Only the host thread touches this.
    Vector<NonnullRefPtr<FontServerConnection>> m_connections;

    NonnullRefPtr<Threading::Thread> m_thread;
};

}
