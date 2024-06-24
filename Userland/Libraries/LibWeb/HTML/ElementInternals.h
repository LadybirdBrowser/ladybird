/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#elementinternals
class ElementInternals final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ElementInternals, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(ElementInternals);

public:
    static JS::NonnullGCPtr<ElementInternals> create(JS::Realm&, HTMLElement& target_element);

    JS::GCPtr<DOM::ShadowRoot> shadow_root() const;

private:
    explicit ElementInternals(JS::Realm&, HTMLElement& target_element);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#internals-target
    JS::NonnullGCPtr<HTMLElement> m_target_element;
};

}
