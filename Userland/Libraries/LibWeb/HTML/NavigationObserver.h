/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class NavigationObserver final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(NavigationObserver, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(NavigationObserver);

public:
    [[nodiscard]] JS::GCPtr<JS::HeapFunction<void()>> navigation_complete() const { return m_navigation_complete; }
    void set_navigation_complete(Function<void()>);

private:
    NavigationObserver(JS::Realm&, Navigable&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    JS::NonnullGCPtr<Navigable> m_navigable;
    JS::GCPtr<JS::HeapFunction<void()>> m_navigation_complete;
};

}
