/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/RustCSSDescriptors.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(RustCSSDescriptors);

GC::Ref<RustCSSDescriptors> RustCSSDescriptors::create(AtRuleID at_rule, RustDescriptorBlock descriptors)
{
    return GC::Heap::the().allocate<RustCSSDescriptors>(at_rule, move(descriptors));
}

RustCSSDescriptors::RustCSSDescriptors(AtRuleID at_rule, RustDescriptorBlock descriptors)
    : CSSDescriptors(at_rule, move(descriptors))
    , m_at_rule(at_rule)
{
}

void RustCSSDescriptors::set_owner(Bindings::InterfaceName interface_name, Bindings::RustInterfaceCell& owner)
{
    VERIFY(!m_owner);
    m_owner_interface_name = interface_name;
    m_owner = owner;
}

WebIDL::ExceptionOr<void> RustCSSDescriptors::set_property(Utf16FlyString const& property, Utf16View value, Utf16View priority)
{
    TRY(Base::set_property(property, value, priority));

    if (m_at_rule == AtRuleID::FontFace) {
        if (auto* font_face_rule = as_if<CSSFontFaceRule>(parent_rule().ptr()))
            font_face_rule->handle_descriptor_change(property);
    }

    return {};
}

extern "C" JSObject* web_rust_bindings_wrap(JSVM*, void* realm, u16 interface_name, void* cell);

GC::Ref<JS::HostObject> RustCSSDescriptors::create_wrapper(JS::Realm& realm)
{
    VERIFY(m_owner);
    auto* wrapper = web_rust_bindings_wrap(JS::HostABI::vm_to_abi(realm.vm()), &realm, to_underlying(m_owner_interface_name), m_owner.ptr());
    return as<JS::HostObject>(*JS::HostABI::object_from_abi(wrapper));
}

void RustCSSDescriptors::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_owner);
}

// Declared on the Rust side in Libraries/LibWeb/Bindings/Rust/src/css/css_descriptors.rs.
extern "C" {
void web_css_descriptors_set_owner(void* descriptors, u16 interface_name, void* owner);
FlatPtr web_css_descriptors_get(void* descriptors, u8 const* name, size_t name_length);
bool web_css_descriptors_set(void* descriptors, u8 const* name, size_t name_length, FlatPtr value);
}

static RustCSSDescriptors& descriptors_from_cell(void* cell)
{
    return *static_cast<RustCSSDescriptors*>(static_cast<GC::Cell*>(cell));
}

static Utf16FlyString descriptor_name(u8 const* name, size_t name_length)
{
    return Utf16FlyString::from_utf8_without_validation(StringView { name, name_length });
}

void web_css_descriptors_set_owner(void* descriptors, u16 interface_name, void* owner)
{
    descriptors_from_cell(descriptors).set_owner(static_cast<Bindings::InterfaceName>(interface_name), *static_cast<Bindings::RustInterfaceCell*>(owner));
}

FlatPtr web_css_descriptors_get(void* descriptors, u8 const* name, size_t name_length)
{
    return descriptors_from_cell(descriptors).get_property_value(descriptor_name(name, name_length)).into_raw();
}

bool web_css_descriptors_set(void* descriptors, u8 const* name, size_t name_length, FlatPtr value)
{
    auto text = Utf16String::from_raw(value);
    return !descriptors_from_cell(descriptors).set_property(descriptor_name(name, name_length), text.utf16_view(), u""sv).is_error();
}

}
