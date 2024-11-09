/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class WindowProxy final : public JS::Object {
    JS_OBJECT(WindowProxy, JS::Object);
    JS_DECLARE_ALLOCATOR(WindowProxy);

public:
    virtual ~WindowProxy() override = default;

    virtual JS::ThrowCompletionOr<JS::Object*> internal_get_prototype_of() const override;
    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype) override;
    virtual JS::ThrowCompletionOr<bool> internal_is_extensible() const override;
    virtual JS::ThrowCompletionOr<bool> internal_prevent_extensions() override;
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const&) const override;
    virtual JS::ThrowCompletionOr<bool> internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor const&, Optional<JS::PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual JS::ThrowCompletionOr<JS::Value> internal_get(JS::PropertyKey const&, JS::Value receiver, JS::CacheablePropertyMetadata*, PropertyLookupPhase) const override;
    virtual JS::ThrowCompletionOr<bool> internal_set(JS::PropertyKey const&, JS::Value value, JS::Value receiver, JS::CacheablePropertyMetadata*) override;
    virtual JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const&) override;
    virtual JS::ThrowCompletionOr<JS::MarkedVector<JS::Value>> internal_own_property_keys() const override;

    JS::GCPtr<Window> window() const { return m_window; }
    void set_window(JS::NonnullGCPtr<Window>);

    JS::NonnullGCPtr<BrowsingContext> associated_browsing_context() const;

private:
    explicit WindowProxy(JS::Realm&);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // [[Window]], https://html.spec.whatwg.org/multipage/window-object.html#concept-windowproxy-window
    JS::GCPtr<Window> m_window;
};

}
