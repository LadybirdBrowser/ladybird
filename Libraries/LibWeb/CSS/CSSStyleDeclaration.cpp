/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

#if ARCH(X86_64) && !defined(AK_OS_WINDOWS)
static_assert(sizeof(CSSStyleDeclaration) == 48);
#endif

GC_DEFINE_ALLOCATOR(CSSStyleDeclaration);

CSSStyleDeclaration::CSSStyleDeclaration(Computed computed, Readonly readonly)
    : m_computed(computed == Computed::Yes)
    , m_readonly(readonly == Readonly::Yes)
{
}

void CSSStyleDeclaration::set_owner(Bindings::InterfaceName interface_name, Bindings::RustInterfaceCell& owner)
{
    VERIFY(!m_rust_owner);
    m_rust_owner_interface_name = interface_name;
    m_rust_owner = owner;
}

Bindings::RustInterfaceCell& CSSStyleDeclaration::ensure_owner()
{
    if (!m_rust_owner)
        create_owner();
    return *m_rust_owner;
}

extern "C" JSObject* web_rust_bindings_wrap(JSVM*, void* realm, u16 interface_name, void* cell);

GC::Ref<JS::HostObject> CSSStyleDeclaration::create_wrapper(JS::Realm& realm)
{
    auto& owner = ensure_owner();
    auto* wrapper = web_rust_bindings_wrap(JS::HostABI::vm_to_abi(realm.vm()), &realm, to_underlying(m_rust_owner_interface_name), &owner);
    return as<JS::HostObject>(*JS::HostABI::object_from_abi(wrapper));
}

void CSSStyleDeclaration::prepare_to_update_style_attribute()
{
    VERIFY(!is_computed());
    if (!owner_node().has_value()) {
        if (auto rule = parent_rule())
            flush_deferred_style_change_events_for_rule(*rule);
        return;
    }

    // OPTIMIZATION: A geometry read can leave paint-only selector facts pending. An inline
    //               declaration is not a replayable fact, so consume the boundary while the old
    //               declaration block is still authoritative.
    owner_node()->element().document().flush_deferred_style_change_event();
    owner_node()->element().prepare_for_inline_style_change();
}

// https://drafts.csswg.org/cssom/#update-style-attribute-for
void CSSStyleDeclaration::update_style_attribute()
{
    // 1. Assert: declaration block’s computed flag is unset.
    VERIFY(!is_computed());

    // 2. Let owner node be declaration block’s owner node.
    // 3. If owner node is null, then return.
    if (!owner_node().has_value())
        return;

    auto& element = owner_node()->element();
    // OPTIMIZATION: Keep the parsed declaration block authoritative and serialize it only when
    //               something observes the textual attribute value.
    if (element.can_defer_inline_style_attribute_update()) {
        element.did_update_inline_style();
        return;
    }

    // 4. Set declaration block’s updating flag.
    set_is_updating(true);

    // 5. Set an attribute value for owner node using "style" and the result of serializing declaration block.
    element.set_attribute_value(HTML::AttributeNames::style, serialized());

    // 6. Unset declaration block’s updating flag.
    set_is_updating(false);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
Utf16String CSSStyleDeclaration::css_text() const
{
    // 1. If the computed flag is set, then return the empty string.
    if (is_computed())
        return {};

    // 2. Return the result of serializing the declarations.
    return serialized();
}

void CSSStyleDeclaration::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_rule);
    visitor.visit(m_owner_element);
    visitor.visit(m_rust_owner);
}

void CSSStyleDeclaration::set_owner_node(Optional<DOM::AbstractElement> owner_node)
{
    if (!owner_node.has_value()) {
        m_owner_element = nullptr;
        m_owner_pseudo_element = {};
        return;
    }
    m_owner_element = owner_node->element();
    m_owner_pseudo_element = owner_node->pseudo_element();
}

// Declared on the Rust side in Libraries/LibWeb/Bindings/Rust/src/css/css_descriptors.rs.
extern "C" {
void web_css_descriptors_set_owner(void* declaration, u16 interface_name, void* owner);
void* web_css_declaration_owner(void* declaration);
FlatPtr web_css_descriptors_get(void* declaration, u8 const* name, size_t name_length);
bool web_css_descriptors_set(void* declaration, u8 const* name, size_t name_length, FlatPtr value, FlatPtr* error_message);
}

static CSSStyleDeclaration& declaration_from_cell(void* cell)
{
    return *static_cast<CSSStyleDeclaration*>(static_cast<GC::Cell*>(cell));
}

static Utf16FlyString property_name(u8 const* name, size_t name_length)
{
    return Utf16FlyString::from_utf8_without_validation(StringView { name, name_length });
}

void web_css_descriptors_set_owner(void* declaration, u16 interface_name, void* owner)
{
    declaration_from_cell(declaration).set_owner(static_cast<Bindings::InterfaceName>(interface_name), *static_cast<Bindings::RustInterfaceCell*>(owner));
}

void* web_css_declaration_owner(void* declaration)
{
    return &declaration_from_cell(declaration).ensure_owner();
}

FlatPtr web_css_descriptors_get(void* declaration, u8 const* name, size_t name_length)
{
    return declaration_from_cell(declaration).get_property_value(property_name(name, name_length)).into_raw();
}

bool web_css_descriptors_set(void* declaration, u8 const* name, size_t name_length, FlatPtr value, FlatPtr* error_message)
{
    auto text = Utf16String::from_raw(value);
    auto result = declaration_from_cell(declaration).set_property(property_name(name, name_length), text.utf16_view(), u""sv);
    if (!result.is_error())
        return true;
    auto exception = result.release_error();
    auto message = exception.get<GC::Ref<WebIDL::DOMException>>()->message();
    *error_message = Utf16String(message).into_raw();
    return false;
}

}
