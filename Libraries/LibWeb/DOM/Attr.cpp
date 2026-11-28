/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::DOM {

static_assert(is_shell_of_rust_object<Attr, RustFFI::AttrObject>);

GC::Ref<Attr> Attr::create(Document& document, Utf16FlyString local_name, Utf16String value, GC::Ptr<Element> owner_element)
{
    return create(document, QualifiedName(move(local_name), Optional<Utf16FlyString> {}, Optional<Utf16FlyString> {}), move(value), owner_element);
}

GC::Ref<Attr> Attr::create(Document& document, QualifiedName const& qualified_name, Utf16String value, GC::Ptr<Element> owner_element)
{
    return static_cast<Attr&>(*RustFFI::rust_dom_create_attr(&document, qualified_name.identity(), move(value).into_raw(), owner_element.ptr()));
}

Attr::Attr(VtableProbe probe)
    : Node(probe)
    , m_attr_core { .qualified_name = QualifiedName(probe), .value = {}, .owner_element = {} }
{
    // NB: libweb_rust reads an Attr node as the AttrPrefix it starts with.
    static_assert(sizeof(Attr) == sizeof(RustFFI::AttrPrefix));
    static_assert(offsetof(Attr, m_attr_core) == offsetof(RustFFI::AttrPrefix, core));

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(Attr, m_attr_core) >= sizeof(Node));
}

Element* Attr::owner_element()
{
    return m_attr_core.owner_element.ptr();
}

Element const* Attr::owner_element() const
{
    return m_attr_core.owner_element.ptr();
}

Utf16String Attr::value() const
{
    if (!owner_element())
        return m_attr_core.value;
    return Utf16String::adopt_raw(RustFFI::rust_dom_attr_value(this));
}

// https://dom.spec.whatwg.org/#set-an-existing-attribute-value
WebIDL::ExceptionOr<void> Attr::set_value(Utf16String value)
{
    // 1. If attribute’s element is null, then set attribute’s value to value and return.
    if (!owner_element()) {
        m_attr_core.value = move(value);
        return {};
    }

    // 2. Let element be attribute’s element.
    auto const& element = *owner_element();

    // 3. Let verifiedValue be the result of calling get Trusted Types-compliant attribute value with
    //    attribute’s local name, attribute’s namespace, element, and value.
    auto verified_value = TRY(TrustedTypes::get_trusted_types_compliant_attribute_value(
        local_name(),
        namespace_uri(),
        element,
        value));

    // 4. If attribute’s element is null, then set attribute’s value to verifiedValue, and return.
    if (!owner_element()) {
        m_attr_core.value = move(verified_value);
        return {};
    }

    // 5. Change attribute to verifiedValue.
    change_attribute(move(verified_value));

    return {};
}

// https://dom.spec.whatwg.org/#concept-element-attributes-change
void Attr::change_attribute(Utf16String value)
{
    if (auto* element = owner_element()) {
        element->change_attribute_value(*this, move(value));
        return;
    }
    m_attr_core.value = move(value);
}

}
