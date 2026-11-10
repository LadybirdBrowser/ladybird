/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <LibIPC/Forward.h>
#include <LibIPC/RawAttachment.h>
#include <LibIPC/Transport.h>

namespace IPC {

struct FfiThreadSafeMessagePoster {
    void* poster_with_one_reference;
    bool (*try_post)(void* poster, ReadonlyBytes, ReadonlySpan<RawAttachment>);
    void (*ref)(void* poster);
    void (*unref)(void* poster);
};
static_assert(sizeof(FfiThreadSafeMessagePoster) == 4 * sizeof(void*), "FfiThreadSafeMessagePoster of libipc_rust is written to match this by hand");

class ThreadSafeMessagePoster final : public AtomicRefCounted<ThreadSafeMessagePoster> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullRefPtr<ThreadSafeMessagePoster> create(Transport& transport) { return adopt_ref(*new ThreadSafeMessagePoster(transport)); }

    [[nodiscard]] bool try_post(MessageBuffer&);
    void detach_from_transport();

    FfiThreadSafeMessagePoster to_ffi_with_one_reference();

private:
    explicit ThreadSafeMessagePoster(Transport& transport)
        : m_transport(&transport)
    {
    }

    Mutex m_mutex;
    Transport* m_transport { nullptr };
};

}
