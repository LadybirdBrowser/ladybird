/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class DisposableStack final : public Object {
    JS_OBJECT(DisposableStack, Object);
    GC_DECLARE_ALLOCATOR(DisposableStack);

public:
    virtual ~DisposableStack() override = default;

    enum class DisposableState {
        Pending,
        Disposed
    };

    [[nodiscard]] DisposableState disposable_state() const { return m_disposable_state; }
    void set_disposed() { m_disposable_state = DisposableState::Disposed; }

    [[nodiscard]] DisposeCapability const& dispose_capability() const { return m_dispose_capability; }
    [[nodiscard]] DisposeCapability& dispose_capability() { return m_dispose_capability; }

private:
    DisposableStack(DisposeCapability, Object& prototype);

    virtual void visit_edges(Visitor& visitor) override;

    DisposableState m_disposable_state { DisposableState::Pending };
    DisposeCapability m_dispose_capability;
};

}
