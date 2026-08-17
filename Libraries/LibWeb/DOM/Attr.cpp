/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(Attr);

GC::Ref<Attr> Attr::create(Document& document, Utf16FlyString local_name, Utf16String value, GC::Ptr<Element> owner_element)
{
    return GC::Heap::the().allocate<Attr>(document, QualifiedName(move(local_name), Optional<Utf16FlyString> {}, Optional<Utf16FlyString> {}), move(value), owner_element);
}

GC::Ref<Attr> Attr::create(Document& document, QualifiedName qualified_name, Utf16String value, GC::Ptr<Element> owner_element)
{
    return GC::Heap::the().allocate<Attr>(document, move(qualified_name), move(value), owner_element);
}

GC::Ref<Attr> Attr::clone(Document& document) const
{
    return GC::Heap::the().allocate<Attr>(document, m_qualified_name, value(), nullptr);
}

Attr::Attr(Document& document, QualifiedName qualified_name, Utf16String value, GC::Ptr<Element> owner_element)
    : Node(document, NodeType::ATTRIBUTE_NODE)
    , m_qualified_name(move(qualified_name))
    , m_value(move(value))
    , m_owner_element(owner_element)
{
}

void Attr::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_owner_element);
}

Element* Attr::owner_element()
{
    return m_owner_element.ptr();
}

Element const* Attr::owner_element() const
{
    return m_owner_element.ptr();
}

void Attr::set_owner_element(Element* owner_element)
{
    m_owner_element = owner_element;
}

Utf16String Attr::value() const
{
    if (auto* element = owner_element()) {
        auto index = element->find_attribute_index_ns(namespace_uri(), local_name());
        VERIFY(index.has_value());
        return element->m_attributes->at(*index).value;
    }
    return m_value;
}

void Attr::detach_from_element(Utf16String value)
{
    m_value = move(value);
    m_owner_element = nullptr;
}

// https://dom.spec.whatwg.org/#set-an-existing-attribute-value
WebIDL::ExceptionOr<void> Attr::set_value(Utf16String value)
{
    // 1. If attribute’s element is null, then set attribute’s value to value and return.
    if (!owner_element()) {
        m_value = move(value);
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
        m_value = move(verified_value);
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
    m_value = move(value);
}

}
