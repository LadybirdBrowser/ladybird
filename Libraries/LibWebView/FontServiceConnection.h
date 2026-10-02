/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/Error.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/Forward.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// One dedicated connection to the font service, answered on a thread of its own.
//
// The UI process makes synchronous calls into the Compositor and into each renderer, so a font
// question one of them asks while the UI is waiting on it would deadlock if the UI's main thread
// had to answer it. The Compositor gets one of these, and so does each renderer's render side.
class WEBVIEW_API FontServiceConnection final : public AtomicRefCounted<FontServiceConnection> {
    AK_MAKE_NONCOPYABLE(FontServiceConnection);
    AK_MAKE_NONMOVABLE(FontServiceConnection);

public:
    static ErrorOr<NonnullRefPtr<FontServiceConnection>> create(FontService&);
    ~FontServiceConnection();

    IPC::TransportHandle take_transport_handle();

private:
    explicit FontServiceConnection(FontService&);
    intptr_t thread_main();

    NonnullRefPtr<FontService> m_font_service;
    NonnullRefPtr<Threading::Thread> m_thread;
    Mutex m_mutex;
    ConditionVariable m_initialization_condition { m_mutex };
    bool m_initialized { false };
    Optional<Error> m_initialization_error;
    Optional<IPC::TransportHandle> m_transport_handle;
    RefPtr<Core::WeakEventLoopReference> m_event_loop;
};

}
