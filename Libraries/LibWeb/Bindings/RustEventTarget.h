/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::Bindings {

// The EventTarget that an object of an interface implemented in Rust owns, whose wrappers are the object's
// (Libraries/LibWeb/Bindings/Rust/src/bindings/event_target.rs).
class RustEventTarget final : public DOM::EventTarget {
    WEB_NON_IDL_WRAPPABLE(RustEventTarget, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(RustEventTarget);

public:
    // https://dom.spec.whatwg.org/#get-the-parent
    using GetTheParent = RustEventTarget* (*)(RustInterfaceCell * owner);

    static GC::Ref<RustEventTarget> create();

    void set_owner(InterfaceName, RustInterfaceCell&, GetTheParent);

private:
    RustEventTarget() = default;

    virtual EventTarget* get_parent(DOM::Event const&) override;
    virtual bool is_owned_by_rust_interface_object() const override { return true; }
    virtual GC::Ref<JS::HostObject> create_wrapper(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    InterfaceName m_owner_interface_name { InterfaceName::Unknown };
    GC::Ptr<RustInterfaceCell> m_owner;
    GetTheParent m_get_the_parent { nullptr };
};

// The Event that an object of an interface implemented in Rust owns, whose wrappers are the object's.
class RustEvent final : public DOM::Event {
    WEB_NON_IDL_WRAPPABLE(RustEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(RustEvent);

public:
    static GC::Ref<RustEvent> create(Utf16FlyString const& type, DOM::EventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    void set_owner(InterfaceName, RustInterfaceCell&);

private:
    RustEvent(Utf16FlyString const& type, DOM::EventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual bool is_owned_by_rust_interface_object() const override { return true; }
    virtual GC::Ref<JS::HostObject> create_wrapper(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    InterfaceName m_owner_interface_name { InterfaceName::Unknown };
    GC::Ptr<RustInterfaceCell> m_owner;
};

}
