/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/CSS/CSSDescriptors.h>

namespace Web::CSS {

// The descriptors that an object of an interface implemented in Rust owns, whose wrappers are the object's
// (Libraries/LibWeb/Bindings/Rust/src/css/css_descriptors.rs).
class RustCSSDescriptors final : public CSSDescriptors {
    WEB_NON_IDL_WRAPPABLE(RustCSSDescriptors, CSSDescriptors);
    GC_DECLARE_ALLOCATOR(RustCSSDescriptors);

public:
    [[nodiscard]] static GC::Ref<RustCSSDescriptors> create(AtRuleID, RustDescriptorBlock);

    void set_owner(Bindings::InterfaceName, Bindings::RustInterfaceCell&);

    virtual WebIDL::ExceptionOr<void> set_property(Utf16FlyString const& property, Utf16View value, Utf16View priority) override;

private:
    RustCSSDescriptors(AtRuleID, RustDescriptorBlock);

    virtual bool is_owned_by_rust_interface_object() const override { return true; }
    virtual GC::Ref<JS::HostObject> create_wrapper(JS::Realm&) override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    AtRuleID m_at_rule;
    Bindings::InterfaceName m_owner_interface_name { Bindings::InterfaceName::Unknown };
    GC::Ptr<Bindings::RustInterfaceCell> m_owner;
};

}
