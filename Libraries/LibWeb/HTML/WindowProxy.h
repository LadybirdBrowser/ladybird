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
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

extern WEB_API JSHostClass const window_proxy_host_class;

// 7.2.3 The WindowProxy exotic object, https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-windowproxy-exotic-object
// The exotic object is a host object whose internal methods are those of a WindowProxy. This cell is its companion: it
// holds the internal slots, and the object and the cell keep each other alive.
class WEB_API WindowProxy final : public JS::Cell {
    GC_CELL(WindowProxy, JS::Cell);
    GC_DECLARE_ALLOCATOR(WindowProxy);

public:
    // Scripts see object(), never this cell.
    using JSValueConversionIsForbidden = void;

    static GC::Ref<WindowProxy> create(JS::Realm&);

    // The WindowProxy whose exotic object this is, or null for any other object.
    static WindowProxy* from_object(JS::Object const& object)
    {
        // Every platform object is a JS::HostObject, so its host class is read here without a virtual call. Every
        // generated binding reaches this through Bindings::this_value_realm().
        if (!object.is_platform_object())
            return nullptr;
        auto const& host_object = static_cast<JS::HostObject const&>(object);
        if (&host_object.host_class() != &window_proxy_host_class)
            return nullptr;
        return static_cast<WindowProxy*>(host_object.host_data().ptr());
    }

    virtual ~WindowProxy() override = default;

    JS::HostObject& object() const { return *m_object; }
    JS::Realm& realm() const;

    GC::Ptr<Window> window() const { return m_window; }
    void set_window(GC::Ref<Window>);
    GC::Ptr<RemoteWindow> remote_window() const { return m_remote_window; }
    void set_window(GC::Ref<RemoteWindow>);
    // The [[Window]] is a provisional navigable's, standing in until the document it populates activates. Scripts
    // keep reaching the document the remote navigable displays through the proxy until then.
    void set_remote_window_over_provisional_window(GC::Ref<RemoteWindow>);

    GC::Ptr<BrowsingContext> associated_browsing_context() const;
    GC::Ptr<Navigable> navigable() const;

private:
    friend struct WindowProxyHostObjectTraits;

    explicit WindowProxy(GC::Ref<JS::HostObject>);

    JS::ThrowCompletionOr<JS::Object*> internal_get_prototype_of() const;
    JS::ThrowCompletionOr<bool> internal_set_prototype_of(JS::Object* prototype);
    JS::ThrowCompletionOr<bool> internal_is_extensible() const;
    JS::ThrowCompletionOr<bool> internal_prevent_extensions();
    JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const&) const;
    JS::ThrowCompletionOr<bool> internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor&);
    JS::ThrowCompletionOr<JS::Value> internal_get(JS::PropertyKey const&, JS::Value receiver, JS::CacheableGetPropertyMetadata*, JS::Object::PropertyLookupPhase) const;
    JS::ThrowCompletionOr<bool> internal_set(JS::PropertyKey const&, JS::Value value, JS::Value receiver);
    JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const&);
    JS::ThrowCompletionOr<GC::RootVector<JS::Value>> internal_own_property_keys() const;

    Bindings::PlatformObject& cross_origin_window_wrapper() const;

    bool is_platform_object_same_origin() const;
    Vector<GC::Root<Navigable>> document_tree_child_navigables() const;
    OrderedHashMap<Utf16FlyString, GC::Ref<Navigable>> document_tree_child_navigable_target_name_property_set() const;
    Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper(JS::PropertyKey const&) const;
    GC::RootVector<JS::Value> cross_origin_own_property_keys() const;

    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<JS::HostObject> m_object;

    // [[Window]], https://html.spec.whatwg.org/multipage/window-object.html#concept-windowproxy-window
    // A Window of this process, or the RemoteWindow standing for one hosted by another.
    GC::Ptr<Window> m_window;
    GC::Ptr<RemoteWindow> m_remote_window;

    // Keeps the per-realm Window wrapper alive while cross-origin property descriptors cached on it can be reused
    // through this WindowProxy.
    mutable GC::Ptr<Bindings::PlatformObject> m_cross_origin_window_wrapper;
};

}
