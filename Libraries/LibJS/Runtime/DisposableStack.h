/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class DisposableStack final : public Object {
    JS_OBJECT(DisposableStack, Object);
    JS_DECLARE_ALLOCATOR(DisposableStack);

public:
    virtual ~DisposableStack() override = default;

    enum class DisposableState {
        Pending,
        Disposed
    };

    [[nodiscard]] DisposableState disposable_state() const { return m_state; }
    [[nodiscard]] Vector<DisposableResource> const& disposable_resource_stack() const { return m_disposable_resource_stack; }
    [[nodiscard]] Vector<DisposableResource>& disposable_resource_stack() { return m_disposable_resource_stack; }

    void set_disposed() { m_state = DisposableState::Disposed; }

private:
    DisposableStack(Vector<DisposableResource> stack, Object& prototype);

    virtual void visit_edges(Visitor& visitor) override;

    Vector<DisposableResource> m_disposable_resource_stack;
    DisposableState m_state { DisposableState::Pending };
};

}
