/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/SVG/SVGDescElement.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGTitleElement.h>
#include <LibWeb/SVG/SVGUseElement.h>

namespace Web::SVG {

SVGElement::SVGElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : Element(document, move(qualified_name))
{
}

void SVGElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGElement);
    Base::initialize(realm);
}

struct NamedPropertyID {
    NamedPropertyID(CSS::PropertyID property_id)
        : id(property_id)
        , name(CSS::string_from_property_id(property_id))
    {
    }

    CSS::PropertyID id;
    StringView name;
};

static Array const attribute_style_properties {
    // FIXME: The `fill` attribute and CSS `fill` property are not the same! But our support is limited enough that they are equivalent for now.
    NamedPropertyID(CSS::PropertyID::Fill),
    // FIXME: The `stroke` attribute and CSS `stroke` property are not the same! But our support is limited enough that they are equivalent for now.
    NamedPropertyID(CSS::PropertyID::ClipPath),
    NamedPropertyID(CSS::PropertyID::ClipRule),
    NamedPropertyID(CSS::PropertyID::Color),
    NamedPropertyID(CSS::PropertyID::Cursor),
    NamedPropertyID(CSS::PropertyID::Direction),
    NamedPropertyID(CSS::PropertyID::Display),
    NamedPropertyID(CSS::PropertyID::FillOpacity),
    NamedPropertyID(CSS::PropertyID::FillRule),
    NamedPropertyID(CSS::PropertyID::FontFamily),
    NamedPropertyID(CSS::PropertyID::FontSize),
    NamedPropertyID(CSS::PropertyID::FontStyle),
    NamedPropertyID(CSS::PropertyID::FontWeight),
    NamedPropertyID(CSS::PropertyID::ImageRendering),
    NamedPropertyID(CSS::PropertyID::LetterSpacing),
    NamedPropertyID(CSS::PropertyID::Mask),
    NamedPropertyID(CSS::PropertyID::MaskType),
    NamedPropertyID(CSS::PropertyID::Opacity),
    NamedPropertyID(CSS::PropertyID::Overflow),
    NamedPropertyID(CSS::PropertyID::PointerEvents),
    NamedPropertyID(CSS::PropertyID::StopColor),
    NamedPropertyID(CSS::PropertyID::StopOpacity),
    NamedPropertyID(CSS::PropertyID::Stroke),
    NamedPropertyID(CSS::PropertyID::StrokeDasharray),
    NamedPropertyID(CSS::PropertyID::StrokeDashoffset),
    NamedPropertyID(CSS::PropertyID::StrokeLinecap),
    NamedPropertyID(CSS::PropertyID::StrokeLinejoin),
    NamedPropertyID(CSS::PropertyID::StrokeMiterlimit),
    NamedPropertyID(CSS::PropertyID::StrokeOpacity),
    NamedPropertyID(CSS::PropertyID::StrokeWidth),
    NamedPropertyID(CSS::PropertyID::TextAnchor),
    NamedPropertyID(CSS::PropertyID::TextOverflow),
    NamedPropertyID(CSS::PropertyID::TransformOrigin),
    NamedPropertyID(CSS::PropertyID::UnicodeBidi),
    NamedPropertyID(CSS::PropertyID::Visibility),
    NamedPropertyID(CSS::PropertyID::WhiteSpace),
    NamedPropertyID(CSS::PropertyID::WordSpacing),
    NamedPropertyID(CSS::PropertyID::WritingMode),
};

bool SVGElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return any_of(attribute_style_properties, [&](auto& property) { return name.equals_ignoring_ascii_case(property.name); });
}

void SVGElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    CSS::Parser::ParsingParams parsing_context { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    for_each_attribute([&](auto& name, auto& value) {
        for (auto property : attribute_style_properties) {
            if (!name.equals_ignoring_ascii_case(property.name))
                continue;
            if (property.id == CSS::PropertyID::Mask) {
                // Mask is a shorthand property in CSS, but parse_css_value does not take that into account. For now,
                // just parse as 'mask-image' as anything else is currently not supported.
                // FIXME: properly parse longhand 'mask' property
                if (auto style_value = parse_css_value(parsing_context, value, CSS::PropertyID::MaskImage)) {
                    cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MaskImage, style_value.release_nonnull());
                }
            } else {
                if (auto style_value = parse_css_value(parsing_context, value, property.id))
                    cascaded_properties->set_property_from_presentational_hint(property.id, style_value.release_nonnull());
            }
            break;
        }
    });
}

bool SVGElement::should_include_in_accessibility_tree() const
{
    bool has_title_or_desc = false;
    auto role = role_from_role_attribute_value();
    for_each_child_of_type<SVGElement>([&has_title_or_desc](auto& child) {
        if ((is<SVGTitleElement>(child) || is<SVGDescElement>(child)) && !child.text_content()->trim_ascii_whitespace().value().is_empty()) {
            has_title_or_desc = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    // https://w3c.github.io/svg-aam/#include_elements
    // TODO: Add support for the SVG tabindex attribute, and include a check for it here.
    return has_title_or_desc
        || (aria_label().has_value() && !aria_label().value().trim_ascii_whitespace().value().is_empty())
        || (aria_labelled_by().has_value() && !aria_labelled_by().value().trim_ascii_whitespace().value().is_empty())
        || (aria_described_by().has_value() && !aria_described_by().value().trim_ascii_whitespace().value().is_empty())
        || (role.has_value() && ARIA::is_abstract_role(role.value()) && role != ARIA::Role::none && role != ARIA::Role::presentation);
}

Optional<ARIA::Role> SVGElement::default_role() const
{
    // https://w3c.github.io/svg-aam/#mapping_role_table
    if (local_name() == TagNames::a && (has_attribute(SVG::AttributeNames::href) || has_attribute(AttributeNames::xlink_href)))
        return ARIA::Role::link;
    if (local_name().is_one_of(TagNames::foreignObject, TagNames::g)
        && should_include_in_accessibility_tree())
        return ARIA::Role::group;
    if (local_name() == TagNames::image && should_include_in_accessibility_tree())
        return ARIA::Role::image;
    if (local_name() == TagNames::circle && should_include_in_accessibility_tree())
        return ARIA::Role::graphicssymbol;
    if (local_name().is_one_of(TagNames::ellipse, TagNames::path, TagNames::polygon, TagNames::polyline)
        && should_include_in_accessibility_tree())
        return ARIA::Role::graphicssymbol;
    return ARIA::Role::generic;
}

void SVGElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    HTMLOrSVGElement::visit_edges(visitor);
    visitor.visit(m_class_name_animated_string);
}

void SVGElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);
    HTMLOrSVGElement::attribute_changed(local_name, old_value, value, namespace_);

    update_use_elements_that_reference_this();
}

WebIDL::ExceptionOr<void> SVGElement::cloned(DOM::Node& copy, bool clone_children) const
{
    TRY(Base::cloned(copy, clone_children));
    TRY(HTMLOrSVGElement::cloned(copy, clone_children));
    return {};
}

void SVGElement::inserted()
{
    Base::inserted();
    HTMLOrSVGElement::inserted();

    update_use_elements_that_reference_this();
}

void SVGElement::children_changed(ChildrenChangedMetadata const* metadata)
{
    Base::children_changed(metadata);

    update_use_elements_that_reference_this();
}

void SVGElement::update_use_elements_that_reference_this()
{
    if (is<SVGUseElement>(this)
        // If this element is in a shadow root, it already represents a clone and is not itself referenced.
        || is<DOM::ShadowRoot>(this->root())
        // If this does not have an id it cannot be referenced, no point in searching the entire DOM tree.
        || !id().has_value()
        // An unconnected node cannot have valid references.
        // This also prevents searches for elements that are in the process of being constructed - as clones.
        || !this->is_connected()
        // Each use element already listens for the completely_loaded event and then clones its reference,
        // we do not have to also clone it in the process of initial DOM building.
        || !document().is_completely_loaded()) {

        return;
    }

    document().for_each_in_subtree_of_type<SVGUseElement>([this](SVGUseElement& use_element) {
        use_element.svg_element_changed(*this);
        return TraversalDecision::Continue;
    });
}

void SVGElement::removed_from(Node* old_parent, Node& old_root)
{
    Base::removed_from(old_parent, old_root);

    remove_from_use_element_that_reference_this();
}

void SVGElement::remove_from_use_element_that_reference_this()
{
    if (is<SVGUseElement>(this) || !id().has_value()) {
        return;
    }

    document().for_each_in_subtree_of_type<SVGUseElement>([this](SVGUseElement& use_element) {
        use_element.svg_element_removed(*this);
        return TraversalDecision::Continue;
    });
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGElement__classNames
GC::Ref<SVGAnimatedString> SVGElement::class_name()
{
    // The className IDL attribute reflects the ‘class’ attribute.
    if (!m_class_name_animated_string)
        m_class_name_animated_string = SVGAnimatedString::create(realm(), *this, AttributeNames::class_);

    return *m_class_name_animated_string;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGElement__ownerSVGElement
GC::Ptr<SVGSVGElement> SVGElement::owner_svg_element()
{
    // The ownerSVGElement IDL attribute represents the nearest ancestor ‘svg’ element.
    // On getting ownerSVGElement, the nearest ancestor ‘svg’ element is returned;
    // if the current element is the outermost svg element, then null is returned.
    return shadow_including_first_ancestor_of_type<SVGSVGElement>();
}

GC::Ref<SVGAnimatedLength> SVGElement::svg_animated_length_for_property(CSS::PropertyID property) const
{
    // FIXME: Create a proper animated value when animations are supported.
    auto make_length = [&] {
        if (auto const computed_properties = this->computed_properties()) {
            if (auto length = computed_properties->length_percentage(property); length.has_value())
                return SVGLength::from_length_percentage(realm(), *length);
        }
        return SVGLength::create(realm(), 0, 0.0f);
    };
    return SVGAnimatedLength::create(realm(), make_length(), make_length());
}

}
