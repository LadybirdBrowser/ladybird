/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Alexander Narsudinov <a.narsudinov@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(NamedNodeMap);

static bool contains_ascii_uppercase(Utf16View string)
{
    for (auto code_unit : string) {
        if (is_ascii_upper_alpha(code_unit))
            return true;
    }
    return false;
}

GC::Ref<NamedNodeMap> NamedNodeMap::create(Element& element)
{
    return GC::Heap::the().allocate<NamedNodeMap>(element);
}

NamedNodeMap::NamedNodeMap(Element& element)
    : m_element(element)
{
}

void NamedNodeMap::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
    visitor.visit(m_attribute_nodes);
}

size_t NamedNodeMap::length() const
{
    return associated_element().attribute_list_size();
}

GC::Ptr<Attr> NamedNodeMap::find_attribute_node(QualifiedName const& name) const
{
    for (auto& attribute : m_attribute_nodes) {
        if (attribute->namespace_uri() == name.namespace_() && attribute->local_name() == name.local_name())
            return attribute;
    }
    return nullptr;
}

GC::Ref<Attr> NamedNodeMap::ensure_attribute_node(QualifiedName const& name) const
{
    if (auto attribute = find_attribute_node(name))
        return *attribute;
    auto& element = const_cast<Element&>(associated_element());
    auto attribute = Attr::create(element.document(), name, {}, element);
    m_attribute_nodes.append(attribute);
    return attribute;
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names%E2%91%A0
Vector<Utf16FlyString> NamedNodeMap::supported_property_names() const
{
    // 1. Let names be the qualified names of the attributes in this NamedNodeMap object’s attribute list, with duplicates omitted, in order.
    Vector<Utf16FlyString> names;
    names.ensure_capacity(length());

    for (size_t index = 0; index < length(); ++index) {
        auto name = associated_element().m_attributes->at(index).name.as_string();
        if (!names.contains_slow(name))
            names.append(move(name));
    }

    // 2. If this NamedNodeMap object’s element is in the HTML namespace and its node document is an HTML document, then for each name of names:
    if (associated_element().namespace_uri() == Namespace::HTML && associated_element().document().is_html_document()) {
        // 1. Let lowercaseName be name, in ASCII lowercase.
        // 2. If lowercaseName is not equal to name, remove name from names.
        names.remove_all_matching([](auto const& name) { return contains_ascii_uppercase(name.view()); });
    }

    // 3. Return names.
    return names;
}

// https://dom.spec.whatwg.org/#dom-namednodemap-item
GC::Ptr<Attr> NamedNodeMap::item(u32 index) const
{
    // 1. If index is equal to or greater than this’s attribute list’s size, then return null.
    if (index >= length())
        return nullptr;

    // 2. Otherwise, return this’s attribute list[index].
    auto name = associated_element().m_attributes->at(index).name;
    return ensure_attribute_node(move(name));
}

// https://dom.spec.whatwg.org/#dom-namednodemap-getnameditem
GC::Ptr<Attr> NamedNodeMap::get_named_item(Utf16FlyString const& qualified_name) const
{
    return get_attribute(qualified_name);
}

// https://dom.spec.whatwg.org/#dom-namednodemap-getnameditemns
GC::Ptr<Attr> NamedNodeMap::get_named_item_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name) const
{
    return get_attribute_ns(namespace_, local_name);
}

// https://dom.spec.whatwg.org/#dom-namednodemap-setnameditem
WebIDL::ExceptionOr<GC::Ptr<Attr>> NamedNodeMap::set_named_item(Attr& attribute)
{
    return set_attribute(GC::Ref { attribute });
}

// https://dom.spec.whatwg.org/#dom-namednodemap-setnameditemns
WebIDL::ExceptionOr<GC::Ptr<Attr>> NamedNodeMap::set_named_item_ns(Attr& attribute)
{
    return set_attribute(GC::Ref { attribute });
}

// https://dom.spec.whatwg.org/#dom-namednodemap-removenameditem
WebIDL::ExceptionOr<GC::Ref<Attr>> NamedNodeMap::remove_named_item(Utf16FlyString const& qualified_name)
{
    // 1. Let attr be the result of removing an attribute given qualifiedName and element.
    auto attribute = remove_attribute(qualified_name);

    // 2. If attr is null, then throw a "NotFoundError" DOMException.
    if (!attribute)
        return WebIDL::NotFoundError::create(Utf16String::formatted("Attribute with name '{}' not found", qualified_name));

    // 3. Return attr.
    return GC::Ref { *attribute };
}

// https://dom.spec.whatwg.org/#dom-namednodemap-removenameditemns
WebIDL::ExceptionOr<GC::Ref<Attr>> NamedNodeMap::remove_named_item_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name)
{
    // 1. Let attr be the result of removing an attribute given namespace, localName, and element.
    auto attribute = remove_attribute_ns(namespace_, local_name);

    // 2. If attr is null, then throw a "NotFoundError" DOMException.
    if (!attribute)
        return WebIDL::NotFoundError::create(Utf16String::formatted("Attribute with namespace '{}' and local name '{}' not found", namespace_, local_name));

    // 3. Return attr.
    return GC::Ref { *attribute };
}

// https://dom.spec.whatwg.org/#concept-element-attributes-get-by-name
GC::Ptr<Attr> NamedNodeMap::get_attribute(Utf16FlyString const& qualified_name, size_t* item_index) const
{
    if (item_index)
        *item_index = 0;
    auto index = associated_element().find_attribute_index(qualified_name);
    if (!index.has_value())
        return nullptr;
    if (item_index)
        *item_index = *index;
    auto name = associated_element().m_attributes->at(*index).name;
    return ensure_attribute_node(move(name));
}

// https://dom.spec.whatwg.org/#concept-element-attributes-get-by-namespace
GC::Ptr<Attr> NamedNodeMap::get_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name, size_t* item_index) const
{
    if (item_index)
        *item_index = 0;
    auto index = associated_element().find_attribute_index_ns(namespace_, local_name);
    if (!index.has_value())
        return nullptr;
    if (item_index)
        *item_index = *index;
    auto name = associated_element().m_attributes->at(*index).name;
    return ensure_attribute_node(move(name));
}

// https://dom.spec.whatwg.org/#concept-element-attributes-set
WebIDL::ExceptionOr<GC::Ptr<Attr>> NamedNodeMap::set_attribute(GC::Ref<Attr> attribute)
{
    // 1. Let verifiedValue be the result of calling get Trusted Types-compliant attribute value
    //    with attr’s local name, attr’s namespace, element, and attr’s value
    auto const verifiedValue = TRY(TrustedTypes::get_trusted_types_compliant_attribute_value(
        attribute->local_name(),
        attribute->namespace_uri(),
        associated_element(),
        attribute->value()));

    // 2. If attr’s element is neither null nor element, throw an "InUseAttributeError" DOMException.
    if ((attribute->owner_element() != nullptr) && (attribute->owner_element() != &associated_element()))
        return WebIDL::InUseAttributeError::create("Attribute must not already be in use"_utf16);

    // 3. Let oldAttr be the result of getting an attribute given attr’s namespace, attr’s local name, and element.
    size_t old_attribute_index = 0;
    auto old_attribute = get_attribute_ns(attribute->namespace_uri(), attribute->local_name(), &old_attribute_index);

    // 4. If oldAttr is attr, return attr.
    if (old_attribute == attribute)
        return attribute;

    // 5. Set attr’s value to verifiedValue.
    TRY(attribute->set_value(verifiedValue));

    // 6. If oldAttr is non-null, then replace oldAttr with attr.
    if (old_attribute) {
        replace_attribute(*old_attribute, attribute, old_attribute_index);
    }
    // 7. Otherwise, append attr to element.
    else {
        append_attribute(attribute);
    }

    // 8. Return oldAttr.
    return old_attribute;
}

// https://dom.spec.whatwg.org/#concept-element-attributes-replace
void NamedNodeMap::replace_attribute(GC::Ref<Attr> old_attribute, GC::Ref<Attr> new_attribute, size_t old_attribute_index)
{
    VERIFY(old_attribute->owner_element());

    // 1. Let element be oldAttribute’s element.
    auto* element = old_attribute->owner_element();

    // 2. Replace oldAttribute by newAttribute in element’s attribute list.
    auto old_value = element->m_attributes->at(old_attribute_index).value;
    auto new_name = new_attribute->m_qualified_name;
    auto new_value = new_attribute->value();
    element->m_attributes->at(old_attribute_index) = { new_name, new_value };

    // 3. Set newAttribute’s element to element.
    new_attribute->set_owner_element(element);

    // 4. Set newAttribute’s node document to element’s node document.
    new_attribute->set_document(Badge<NamedNodeMap> {}, element->document());

    // 5. Set oldAttribute’s element to null.
    detach_attribute_node(old_attribute->m_qualified_name, old_value);
    m_attribute_nodes.append(new_attribute);

    // 6. Handle attribute changes for oldAttribute with element, oldAttribute’s value, and newAttribute’s value.
    element->handle_attribute_changes(move(new_name), move(old_value), move(new_value));
}

// https://dom.spec.whatwg.org/#concept-element-attributes-append
void NamedNodeMap::append_attribute(GC::Ref<Attr> attribute)
{
    // 1. Append attribute to element’s attribute list.
    auto value = attribute->value();
    associated_element().ensure_attribute_list().empend(attribute->m_qualified_name, value);

    // 2. Set attribute’s element to element.
    attribute->set_owner_element(&associated_element());

    // 3. Set attribute’s node document to element’s node document.
    attribute->set_document(Badge<NamedNodeMap> {}, associated_element().document());
    m_attribute_nodes.append(attribute);

    // 4. Handle attribute changes for attribute with element, null, and attribute’s value.
    associated_element().handle_attribute_changes(attribute->m_qualified_name, {}, value);
}

void NamedNodeMap::detach_attribute_node(QualifiedName const& name, Utf16String value)
{
    for (size_t index = 0; index < m_attribute_nodes.size(); ++index) {
        auto& attribute = m_attribute_nodes[index];
        if (attribute->namespace_uri() != name.namespace_() || attribute->local_name() != name.local_name())
            continue;
        attribute->detach_from_element(move(value));
        m_attribute_nodes.remove(index);
        return;
    }
}

// https://dom.spec.whatwg.org/#concept-element-attributes-remove
void NamedNodeMap::remove_attribute_at_index(size_t attribute_index)
{
    associated_element().remove_attribute_at(attribute_index);
}

// https://dom.spec.whatwg.org/#concept-element-attributes-remove-by-name
GC::Ptr<Attr> NamedNodeMap::remove_attribute(Utf16FlyString const& qualified_name)
{
    size_t item_index = 0;

    // 1. Let attr be the result of getting an attribute given qualifiedName and element.
    auto attribute = get_attribute(qualified_name, &item_index);

    // 2. If attr is non-null, then remove attr.
    if (attribute)
        remove_attribute_at_index(item_index);

    // 3. Return attr.
    return attribute;
}

// https://dom.spec.whatwg.org/#concept-element-attributes-remove-by-namespace
GC::Ptr<Attr> NamedNodeMap::remove_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name)
{
    size_t item_index = 0;

    // 1. Let attr be the result of getting an attribute given namespace, localName, and element.
    auto attribute = get_attribute_ns(namespace_, local_name, &item_index);

    // 2. If attr is non-null, then remove attr.
    if (attribute)
        remove_attribute_at_index(item_index);

    // 3. Return attr.
    return attribute;
}

// https://dom.spec.whatwg.org/#dom-element-removeattributenode
WebIDL::ExceptionOr<GC::Ref<Attr>> NamedNodeMap::remove_attribute_node(GC::Ref<Attr> attr)
{
    // 1. If this’s attribute list does not contain attr, then throw a "NotFoundError" DOMException.
    if (attr->owner_element() != &associated_element())
        return WebIDL::NotFoundError::create("Attribute not found"_utf16);

    auto index = associated_element().find_attribute_index_ns(attr->namespace_uri(), attr->local_name());
    if (!index.has_value())
        return WebIDL::NotFoundError::create("Attribute not found"_utf16);

    // 2. Remove attr.
    remove_attribute_at_index(*index);

    // 3. Return attr.
    return attr;
}

}
