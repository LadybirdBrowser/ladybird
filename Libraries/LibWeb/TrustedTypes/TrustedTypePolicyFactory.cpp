/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/WindowEventHandlers.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/TagNames.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicyFactory);

GC::Ref<TrustedTypePolicyFactory> TrustedTypePolicyFactory::create(JS::Realm& realm)
{
    return realm.create<TrustedTypePolicyFactory>(realm);
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-getattributetype
Optional<String> TrustedTypePolicyFactory::get_attribute_type(String const& tag_name, String& attribute, Optional<String> element_ns, Optional<String> attr_ns)
{
    // 1. Set localName to tagName in ASCII lowercase.
    auto const local_name = tag_name.to_ascii_lowercase();

    // 2. Set attribute to attribute in ASCII lowercase.
    attribute = attribute.to_ascii_lowercase();

    // 3. If elementNs is null or an empty string, set elementNs to HTML namespace.
    if (!element_ns.has_value() || element_ns.value().is_empty())
        element_ns = String { Namespace::HTML };

    // 4. If attrNs is an empty string, set attrNs to null.
    if (attr_ns.has_value() && attr_ns.value().is_empty())
        attr_ns.clear();

    // FIXME: We don't have a method in ElementFactory that can give us the interface name but these are all the cases
    // we care about in the table in get_trusted_type_data_for_attribute function
    // 5. Let interface be the element interface for localName and elementNs.
    String interface;
    if (local_name == HTML::TagNames::iframe && element_ns == Namespace::HTML) {
        interface = "HTMLIFrameElement"_string;
    } else if (local_name == HTML::TagNames::script && element_ns == Namespace::HTML) {
        interface = "HTMLScriptElement"_string;
    } else if (local_name == SVG::TagNames::script && element_ns == Namespace::SVG) {
        interface = "SVGScriptElement"_string;
    } else {
        interface = "Element"_string;
    }

    // 6. Let expectedType be null.
    Optional<String> expected_type {};

    // 7. Set attributeData to the result of Get Trusted Type data for attribute algorithm,
    // with the following arguments, interface as element, attribute, attrNs
    auto const attribute_data = get_trusted_type_data_for_attribute(interface, attribute, attr_ns);

    // 8. If attributeData is not null, then set expectedType to the interface’s name of the value of the fourth member of attributeData.
    if (attribute_data.has_value()) {
        expected_type = attribute_data.value().trusted_type;
    }

    // 9. Return expectedType.
    return expected_type;
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-getpropertytype
Optional<String> TrustedTypePolicyFactory::get_property_type(String const& tag_name, String const& property, Optional<String> element_ns)
{
    // 1. Set localName to tagName in ASCII lowercase.
    auto const local_name = tag_name.to_ascii_lowercase();

    // 2. If elementNs is null or an empty string, set elementNs to HTML namespace.
    if (!element_ns.has_value() || element_ns.value().is_empty())
        element_ns = String { Namespace::HTML };

    // FIXME: We don't have a method in ElementFactory that can give us the interface name but these are all the cases
    // we care about in the table in get_trusted_type_data_for_attribute function
    // 3. Let interface be the element interface for localName and elementNs.
    String interface;
    if (local_name == HTML::TagNames::iframe && element_ns == Namespace::HTML) {
        interface = "HTMLIFrameElement"_string;
    } else if (local_name == HTML::TagNames::script && element_ns == Namespace::HTML) {
        interface = "HTMLScriptElement"_string;
    } else {
        interface = "Element"_string;
    }

    // 4. Let expectedType be null.
    Optional<String> expected_type;

    static Vector<Array<String, 3>> const table {
        { "HTMLIFrameElement"_string, "srcdoc"_string, "TrustedHTML"_string },
        { "HTMLScriptElement"_string, "innerText"_string, "TrustedScript"_string },
        { "HTMLScriptElement"_string, "src"_string, "TrustedScriptURL"_string },
        { "HTMLScriptElement"_string, "text"_string, "TrustedScript"_string },
        { "HTMLScriptElement"_string, "textContent"_string, "TrustedScript"_string },
        { "*"_string, "innerHTML"_string, "TrustedHTML"_string },
        { "*"_string, "outerHTML"_string, "TrustedHTML"_string },
    };

    // 5. Find the row in the following table, where the first column is "*" or interface’s name, and property is in the second column.
    // If a matching row is found, set expectedType to the interface’s name of the value of the third column.
    auto const matching_row = table.first_matching([&interface, &property](auto const& row) {
        return (row[0] == interface || row[0] == "*"sv) && row[1] == property;
    });

    if (matching_row.has_value()) {
        expected_type = matching_row.value()[2];
    }

    // 6. Return expectedType.
    return expected_type;
}

TrustedTypePolicyFactory::TrustedTypePolicyFactory(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void TrustedTypePolicyFactory::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedTypePolicyFactory);
    Base::initialize(realm);
}

// https://w3c.github.io/trusted-types/dist/spec/#abstract-opdef-get-trusted-type-data-for-attribute
Optional<TrustedTypeData> get_trusted_type_data_for_attribute(String const& element, String const& attribute, Optional<String> const& attribute_ns)
{
    // 1. Let data be null.
    Optional<TrustedTypeData const&> data {};

    // 2. If attributeNs is null, and attribute is the name of an event handler content attribute, then:
    if (!attribute_ns.has_value()) {
#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                                                                   \
    if (attribute == HTML::AttributeNames::attribute_name) {                                                                      \
        /* 1. Return (Element, null, attribute, TrustedScript, "Element " + attribute). */                                        \
        return TrustedTypeData { "Element"_string, {}, attribute, "TrustedScript"_string, "Element " #attribute_name ""_string }; \
    }
        ENUMERATE_GLOBAL_EVENT_HANDLERS(__ENUMERATE)
        ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
    }

    static Vector<TrustedTypeData> const table {
        { "HTMLIFrameElement"_string, {}, "srcdoc"_string, "TrustedHTML"_string, "HTMLIFrameElement srcdoc"_string },
        { "HTMLScriptElement"_string, {}, "src"_string, "TrustedScriptURL"_string, "HTMLScriptElement src"_string },
        { "SVGScriptElement"_string, {}, "href"_string, "TrustedScriptURL"_string, "SVGScriptElement href"_string },
        { "SVGScriptElement"_string, Namespace::XLink.to_string(), "href"_string, "TrustedScriptURL"_string, "SVGScriptElement href"_string },
    };

    // 3. Find the row in the following table, where element is in the first column, attributeNs is in the second column,
    // and attribute is in the third column. If a matching row is found, set data to that row.
    data = table.first_matching([&element, &attribute, &attribute_ns](auto const& row) {
        return row.element == element && row.attribute_ns == attribute_ns && row.attribute_local_name == attribute;
    });

    // 4. Return data
    return data.copy();
}

}
