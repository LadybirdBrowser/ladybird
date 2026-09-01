/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibIPC/TransportHandle.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Forward.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API CompositorFontServiceConnection final : public AtomicRefCounted<CompositorFontServiceConnection> {
    AK_MAKE_NONCOPYABLE(CompositorFontServiceConnection);
    AK_MAKE_NONMOVABLE(CompositorFontServiceConnection);

public:
    static ErrorOr<NonnullRefPtr<CompositorFontServiceConnection>> create(FontService&);
    ~CompositorFontServiceConnection();

    IPC::TransportHandle take_transport_handle();

private:
    explicit CompositorFontServiceConnection(FontService&);
    intptr_t thread_main();

    FontService& m_font_service;
    NonnullRefPtr<Threading::Thread> m_thread;
    Sync::Mutex m_mutex;
    Sync::ConditionVariable m_initialization_condition { m_mutex };
    bool m_initialized { false };
    Optional<Error> m_initialization_error;
    Optional<IPC::TransportHandle> m_transport_handle;
    RefPtr<Core::WeakEventLoopReference> m_event_loop;
};

}
