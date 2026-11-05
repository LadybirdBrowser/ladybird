/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
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

// Declared on the Rust side in Libraries/LibWeb/Bindings/Rust/src/css/css_style_declaration.rs and css_rule.rs.
extern "C" {
void web_css_declaration_set_owner(void* declaration, u16 interface_name, void* owner);
void* web_css_declaration_owner(void* declaration);
FlatPtr web_css_declaration_get_by_name(void* declaration, u8 const* name, size_t name_length);
bool web_css_declaration_set_by_name(void* declaration, u8 const* name, size_t name_length, FlatPtr value, FlatPtr* error_message);
FlatPtr web_css_declaration_css_text(void* declaration);
bool web_css_declaration_set_css_text(void* declaration, FlatPtr text, FlatPtr* error_message);
size_t web_css_declaration_length(void* declaration);
FlatPtr web_css_declaration_item(void* declaration, size_t index);
FlatPtr web_css_declaration_get_property_value(void* declaration, FlatPtr property);
FlatPtr web_css_declaration_get_property_priority(void* declaration, FlatPtr property);
bool web_css_declaration_set_property(void* declaration, FlatPtr property, FlatPtr value, FlatPtr priority, FlatPtr* error_message);
bool web_css_declaration_remove_property(void* declaration, FlatPtr property, FlatPtr* removed_value, FlatPtr* error_message);
void* web_css_declaration_parent_rule(void* declaration);
}

static CSSStyleDeclaration& declaration_from_cell(void* cell)
{
    return *static_cast<CSSStyleDeclaration*>(static_cast<GC::Cell*>(cell));
}

// NB: The bindings only pass static names, so each one is interned once.
static Utf16FlyString property_name_from_static_utf8(u8 const* name, size_t name_length)
{
    static NeverDestroyed<HashMap<StringView, Utf16FlyString>> names;
    StringView key { name, name_length };
    return names->ensure(key, [&] { return Utf16FlyString::from_utf8_without_validation(key); });
}

static Utf16FlyString property_name_from_raw(FlatPtr property)
{
    return Utf16FlyString { Utf16String::adopt_raw(property) };
}

// NB: Every step that throws throws a NoModificationAllowedError, which the Rust bindings recreate from its message.
template<typename T>
static bool succeeded_or_store_message(WebIDL::ExceptionOr<T> const& result, FlatPtr* error_message)
{
    if (!result.is_error())
        return true;
    *error_message = result.exception().template get<GC::Ref<WebIDL::DOMException>>()->message().to_utf16_string().into_raw();
    return false;
}

void web_css_declaration_set_owner(void* declaration, u16 interface_name, void* owner)
{
    declaration_from_cell(declaration).set_owner(static_cast<Bindings::InterfaceName>(interface_name), *static_cast<Bindings::RustInterfaceCell*>(owner));
}

void* web_css_declaration_owner(void* declaration)
{
    return &declaration_from_cell(declaration).ensure_owner();
}

FlatPtr web_css_declaration_get_by_name(void* declaration, u8 const* name, size_t name_length)
{
    return declaration_from_cell(declaration).get_property_value(property_name_from_static_utf8(name, name_length)).into_raw();
}

bool web_css_declaration_set_by_name(void* declaration, u8 const* name, size_t name_length, FlatPtr value, FlatPtr* error_message)
{
    auto text = Utf16String::adopt_raw(value);
    return succeeded_or_store_message(declaration_from_cell(declaration).set_property(property_name_from_static_utf8(name, name_length), text.utf16_view(), u""sv), error_message);
}

FlatPtr web_css_declaration_css_text(void* declaration)
{
    return declaration_from_cell(declaration).css_text().into_raw();
}

bool web_css_declaration_set_css_text(void* declaration, FlatPtr text, FlatPtr* error_message)
{
    auto css_text = Utf16String::adopt_raw(text);
    return succeeded_or_store_message(declaration_from_cell(declaration).set_css_text(css_text.utf16_view()), error_message);
}

size_t web_css_declaration_length(void* declaration)
{
    return declaration_from_cell(declaration).length();
}

FlatPtr web_css_declaration_item(void* declaration, size_t index)
{
    return declaration_from_cell(declaration).item(index).into_raw();
}

FlatPtr web_css_declaration_get_property_value(void* declaration, FlatPtr property)
{
    return declaration_from_cell(declaration).get_property_value(property_name_from_raw(property)).into_raw();
}

FlatPtr web_css_declaration_get_property_priority(void* declaration, FlatPtr property)
{
    return declaration_from_cell(declaration).get_property_priority(property_name_from_raw(property)).into_raw();
}

bool web_css_declaration_set_property(void* declaration, FlatPtr property, FlatPtr value, FlatPtr priority, FlatPtr* error_message)
{
    auto value_text = Utf16String::adopt_raw(value);
    auto priority_text = Utf16String::adopt_raw(priority);
    return succeeded_or_store_message(declaration_from_cell(declaration).set_property(property_name_from_raw(property), value_text.utf16_view(), priority_text.utf16_view()), error_message);
}

bool web_css_declaration_remove_property(void* declaration, FlatPtr property, FlatPtr* removed_value, FlatPtr* error_message)
{
    auto result = declaration_from_cell(declaration).remove_property(property_name_from_raw(property));
    if (!succeeded_or_store_message(result, error_message))
        return false;
    *removed_value = result.release_value().into_raw();
    return true;
}

void* web_css_declaration_parent_rule(void* declaration)
{
    return declaration_from_cell(declaration).parent_rule().ptr();
}

}
