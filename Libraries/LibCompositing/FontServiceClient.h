/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ConditionVariable.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibCompositing/Export.h>
#include <LibGfx/Font/SharedFontProvider.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/Forward.h>

namespace Compositing {

class FontServiceConnectionToServer;

// A connection to the UI process's font service that any thread may ask on.
//
// LibIPC pins a connection to the thread that constructed it, and that thread must have a
// Core::EventLoop. This client owns a thread that does nothing else: a caller hands it a question
// and blocks until the answer comes back, and needs neither a connection nor an event loop of its
// own. A renderer's render side asks its font questions here, because the connection the
// renderer's document thread owns is pumped by that thread alone.
class COMPOSITING_API FontServiceClient {
    AK_MAKE_NONCOPYABLE(FontServiceClient);
    AK_MAKE_NONMOVABLE(FontServiceClient);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<FontServiceClient>> create(IPC::TransportHandle);
    ~FontServiceClient();

    // If the font service goes away, every question answers as if nothing matched.
    Gfx::BrokeredFont open_font(u64 generation, u64 face_id);
    Gfx::BrokeredFont match_font(String const& family, u16 weight, u16 width, u8 slope);
    Gfx::BrokeredFont match_local_font(String const& name);
    Gfx::BrokeredFont match_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji);
    Optional<FlyString> resolve_generic_family(String const& family, u16 weight, u8 slope);

private:
    using Question = Function<void(FontServiceConnectionToServer&)>;

    explicit FontServiceClient(IPC::TransportHandle);
    intptr_t thread_main();
    void ask(Question const&);

    // Serializes callers, so that one question is in flight at a time.
    Mutex m_caller_mutex;

    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    Optional<IPC::TransportHandle> m_transport_handle;
    bool m_initialized { false };
    bool m_connected { false };
    bool m_should_quit { false };
    Question const* m_question { nullptr };

    NonnullRefPtr<Threading::Thread> m_thread;
};

}
