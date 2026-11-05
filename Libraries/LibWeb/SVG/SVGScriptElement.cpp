/*
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGScriptElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGScriptElement);

SVGScriptElement::SVGScriptElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGScriptElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visit_script_element_edges(visitor);
}

void SVGScriptElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);
    retarget_script_load_event_delayer(document());
}

void SVGScriptElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::href && (!namespace_.has_value() || namespace_ == Namespace::XLink)) {
        if (value.has_value() && is_connected())
            post_connection();
    }

    script_element_attribute_changed(name);
}

void SVGScriptElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    script_element_children_changed(metadata);
}

void SVGScriptElement::post_connection()
{
    script_element_post_connection();
}

WebIDL::ExceptionOr<void> SVGScriptElement::cloned(Node& copy, bool subtree) const
{
    TRY(Base::cloned(copy, subtree));
    script_element_cloned(as<SVGScriptElement>(copy));
    return {};
}

bool SVGScriptElement::has_source_attribute() const
{
    return has_attribute(SVG::AttributeNames::href) || has_attribute_ns(Namespace::XLink, SVG::AttributeNames::href);
}

Utf16String SVGScriptElement::source_attribute_value()
{
    return href()->base_val();
}

HTML::ScriptElementFetchAttributes SVGScriptElement::fetch_attributes() const
{
    auto crossorigin = get_attribute(HTML::AttributeNames::crossorigin);
    auto referrer_policy_attribute = get_attribute(HTML::AttributeNames::referrerpolicy).value_or({});
    return {
        .crossorigin = HTML::cors_setting_attribute_from_keyword(crossorigin.map([](auto const& value) { return value.utf16_view(); })),
        .cryptographic_nonce = nonce(),
        .referrer_policy = ReferrerPolicy::from_string(referrer_policy_attribute).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString),
        .parser_inserted = is_parser_inserted(),
    };
}

}
