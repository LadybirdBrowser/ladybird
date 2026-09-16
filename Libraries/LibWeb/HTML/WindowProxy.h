/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class WEB_API WindowProxy final : public Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(WindowProxy, Bindings::PlatformObject)
    GC_DECLARE_ALLOCATOR(WindowProxy);

public:
    static GC::Ref<WindowProxy> create(JS::Realm&);
    virtual ~WindowProxy() override = default;

    virtual JS::ThrowCompletionOr<JS::Object*> internal_get_prototype_of() const override;
    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype) override;
    virtual JS::ThrowCompletionOr<bool> internal_is_extensible() const override;
    virtual JS::ThrowCompletionOr<bool> internal_prevent_extensions() override;
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const&) const override;
    virtual JS::ThrowCompletionOr<bool> internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual JS::ThrowCompletionOr<JS::Value> internal_get(JS::PropertyKey const&, JS::Value receiver, JS::CacheableGetPropertyMetadata*, PropertyLookupPhase) const override;
    virtual bool is_cacheable_for_property_absence() const override { return false; }
    virtual JS::ThrowCompletionOr<bool> internal_set(JS::PropertyKey const&, JS::Value value, JS::Value receiver, JS::CacheableSetPropertyMetadata*, PropertyLookupPhase) override;
    virtual JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const&) override;
    virtual JS::ThrowCompletionOr<GC::RootVector<JS::Value>> internal_own_property_keys() const override;

    GC::Ptr<Window> window() const { return m_window; }
    void set_window(GC::Ref<Window>);
    GC::Ptr<RemoteWindow> remote_window() const { return m_remote_window; }
    void set_window(GC::Ref<RemoteWindow>);

    GC::Ptr<BrowsingContext> associated_browsing_context() const;

private:
    explicit WindowProxy(JS::Realm&);
    Bindings::PlatformObject& cross_origin_window_wrapper() const;

    bool is_platform_object_same_origin() const;
    Vector<GC::Root<Navigable>> document_tree_child_navigables() const;
    OrderedHashMap<Utf16FlyString, GC::Ref<Navigable>> document_tree_child_navigable_target_name_property_set() const;
    Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper(JS::PropertyKey const&) const;
    GC::RootVector<JS::Value> cross_origin_own_property_keys() const;

    virtual bool is_html_window_proxy() const override { return true; }
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // [[Window]], https://html.spec.whatwg.org/multipage/window-object.html#concept-windowproxy-window
    // A Window of this process, or the RemoteWindow standing for one hosted by another.
    GC::Ptr<Window> m_window;
    GC::Ptr<RemoteWindow> m_remote_window;

    // Keeps the per-realm Window wrapper alive while cross-origin property descriptors cached on it can be reused
    // through this WindowProxy.
    mutable GC::Ptr<Bindings::PlatformObject> m_cross_origin_window_wrapper;
};

}

template<>
inline bool JS::Object::fast_is<Web::HTML::WindowProxy>() const { return is_html_window_proxy(); }
