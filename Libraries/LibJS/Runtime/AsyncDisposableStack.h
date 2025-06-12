/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class AsyncDisposableStack final : public Object {
    JS_OBJECT(AsyncDisposableStack, Object);
    GC_DECLARE_ALLOCATOR(AsyncDisposableStack);

public:
    virtual ~AsyncDisposableStack() override = default;

    enum class AsyncDisposableState {
        Pending,
        Disposed
    };

    [[nodiscard]] AsyncDisposableState async_disposable_state() const { return m_async_disposable_state; }
    void set_disposed() { m_async_disposable_state = AsyncDisposableState::Disposed; }

    [[nodiscard]] DisposeCapability const& dispose_capability() const { return m_dispose_capability; }
    [[nodiscard]] DisposeCapability& dispose_capability() { return m_dispose_capability; }

private:
    AsyncDisposableStack(DisposeCapability, Object& prototype);

    virtual void visit_edges(Visitor& visitor) override;

    DisposeCapability m_dispose_capability;
    AsyncDisposableState m_async_disposable_state { AsyncDisposableState::Pending };
};

}
