/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGDescElement.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGSymbolElement.h>
#include <LibWeb/SVG/SVGTitleElement.h>
#include <LibWeb/SVG/SVGUseElement.h>
#include <LibWeb/SVG/TagNames.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGElement);

SVGElement::SVGElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : Element(document, move(qualified_name))
{
}

RefPtr<Layout::Node> SVGElement::create_layout_node(CSS::LayoutStyle)
{
    return nullptr;
}

struct NamedPropertyID {
    NamedPropertyID(CSS::PropertyID property_id, Utf16FlyString name, std::initializer_list<Utf16FlyString> supported_elements = {})
        : id(property_id)
        , name(move(name))
        , supported_elements(supported_elements)
    {
    }
    NamedPropertyID(CSS::PropertyID property_id, std::initializer_list<Utf16FlyString> supported_elements = {})
        : NamedPropertyID(property_id, CSS::string_from_property_id(property_id).to_utf16_string(), supported_elements)
    {
    }

    CSS::PropertyID id;
    Utf16FlyString name;
    Vector<Utf16FlyString> supported_elements;
};

static ReadonlySpan<NamedPropertyID> attribute_style_properties()
{
    // https://svgwg.org/svg2-draft/styling.html#PresentationAttributes
    static auto const& properties = *new Array {
        // FIXME: The `fill` attribute and CSS `fill` property are not the same! But our support is limited enough that they are equivalent for now.
        NamedPropertyID(CSS::PropertyID::Fill),
        // FIXME: The `stroke` attribute and CSS `stroke` property are not the same! But our support is limited enough that they are equivalent for now.
        NamedPropertyID(CSS::PropertyID::ClipPath),
        NamedPropertyID(CSS::PropertyID::ClipRule),
        NamedPropertyID(CSS::PropertyID::Color),
        NamedPropertyID(CSS::PropertyID::ColorInterpolation),
        NamedPropertyID(CSS::PropertyID::ColorInterpolationFilters),
        NamedPropertyID(CSS::PropertyID::Cursor),
        NamedPropertyID(CSS::PropertyID::Cx, { SVG::TagNames::circle, SVG::TagNames::ellipse }),
        NamedPropertyID(CSS::PropertyID::Cy, { SVG::TagNames::circle, SVG::TagNames::ellipse }),
        NamedPropertyID(CSS::PropertyID::D, { SVG::TagNames::path }),
        NamedPropertyID(CSS::PropertyID::Direction),
        NamedPropertyID(CSS::PropertyID::Display),
        NamedPropertyID(CSS::PropertyID::DominantBaseline),
        NamedPropertyID(CSS::PropertyID::FillOpacity),
        NamedPropertyID(CSS::PropertyID::FillRule),
        NamedPropertyID(CSS::PropertyID::Filter),
        NamedPropertyID(CSS::PropertyID::FloodColor),
        NamedPropertyID(CSS::PropertyID::FloodOpacity),
        NamedPropertyID(CSS::PropertyID::FontFamily),
        NamedPropertyID(CSS::PropertyID::FontSize),
        NamedPropertyID(CSS::PropertyID::FontStyle),
        NamedPropertyID(CSS::PropertyID::FontVariant),
        NamedPropertyID(CSS::PropertyID::FontWeight),
        NamedPropertyID(CSS::PropertyID::FontWidth, "font-stretch"_utf16_fly_string),
        NamedPropertyID(CSS::PropertyID::Height, { SVG::TagNames::foreignObject, SVG::TagNames::image, SVG::TagNames::rect, SVG::TagNames::svg, SVG::TagNames::symbol, SVG::TagNames::use }),
        NamedPropertyID(CSS::PropertyID::ImageRendering),
        NamedPropertyID(CSS::PropertyID::LetterSpacing),
        NamedPropertyID(CSS::PropertyID::Mask),
        NamedPropertyID(CSS::PropertyID::MaskType),
        NamedPropertyID(CSS::PropertyID::Opacity),
        NamedPropertyID(CSS::PropertyID::Overflow),
        NamedPropertyID(CSS::PropertyID::PaintOrder),
        NamedPropertyID(CSS::PropertyID::PointerEvents),
        NamedPropertyID(CSS::PropertyID::R, { SVG::TagNames::circle }),
        NamedPropertyID(CSS::PropertyID::Rx, { SVG::TagNames::ellipse, SVG::TagNames::rect }),
        NamedPropertyID(CSS::PropertyID::Ry, { SVG::TagNames::ellipse, SVG::TagNames::rect }),
        NamedPropertyID(CSS::PropertyID::ShapeRendering),
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
        NamedPropertyID(CSS::PropertyID::VectorEffect),
        NamedPropertyID(CSS::PropertyID::TextAnchor),
        NamedPropertyID(CSS::PropertyID::TextDecoration),
        NamedPropertyID(CSS::PropertyID::TextRendering),
        NamedPropertyID(CSS::PropertyID::TextOverflow),
        NamedPropertyID(CSS::PropertyID::Transform),
        NamedPropertyID(CSS::PropertyID::Transform, "gradientTransform"_utf16_fly_string, { SVG::TagNames::linearGradient, SVG::TagNames::radialGradient }),
        NamedPropertyID(CSS::PropertyID::Transform, "patternTransform"_utf16_fly_string, { SVG::TagNames::pattern }),
        NamedPropertyID(CSS::PropertyID::TransformOrigin),
        NamedPropertyID(CSS::PropertyID::UnicodeBidi),
        NamedPropertyID(CSS::PropertyID::Visibility),
        NamedPropertyID(CSS::PropertyID::WhiteSpace),
        NamedPropertyID(CSS::PropertyID::Width, { SVG::TagNames::foreignObject, SVG::TagNames::image, SVG::TagNames::rect, SVG::TagNames::svg, SVG::TagNames::symbol, SVG::TagNames::use }),
        NamedPropertyID(CSS::PropertyID::WordSpacing),
        NamedPropertyID(CSS::PropertyID::WritingMode),
        NamedPropertyID(CSS::PropertyID::X, { SVG::TagNames::foreignObject, SVG::TagNames::image, SVG::TagNames::rect, SVG::TagNames::svg, SVG::TagNames::symbol, SVG::TagNames::use }),
        NamedPropertyID(CSS::PropertyID::Y, { SVG::TagNames::foreignObject, SVG::TagNames::image, SVG::TagNames::rect, SVG::TagNames::svg, SVG::TagNames::symbol, SVG::TagNames::use }),
    };
    return properties;
}

static Optional<CSS::PropertyID> property_id_for_presentational_attribute(Utf16FlyString const& name, Utf16FlyString const& tag_name)
{
    // https://svgwg.org/svg2-draft/styling.html#PresentationAttributes
    // The pattern and gradient elements take patternTransform and gradientTransform instead of the
    // plain transform presentation attribute.
    if (name == "transform"_utf16_fly_string
        && (tag_name == TagNames::pattern || tag_name == TagNames::linearGradient || tag_name == TagNames::radialGradient))
        return {};

    for (auto const& property : attribute_style_properties()) {
        if (!property.name.equals_ignoring_ascii_case(name))
            continue;

        if (!property.supported_elements.is_empty() && !property.supported_elements.contains_slow(tag_name))
            continue;

        return property.id;
    }

    return {};
}

bool SVGElement::is_presentational_hint(Utf16FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return property_id_for_presentational_attribute(name, local_name()).has_value();
}

void SVGElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);

    if (m_presentation_attribute_style.has_value()) {
        properties.extend(*m_presentation_attribute_style);
        return;
    }

    Vector<CSS::StyleProperty> presentation_attribute_style;
    for_each_attribute([&](DOM::QualifiedName const& name, Utf16View const& value) {
        if (name.namespace_().has_value())
            return;
        if (auto property_id = property_id_for_presentational_attribute(name.as_string(), local_name()); property_id.has_value()) {
            auto style_value = parse_presentation_attribute(*property_id, value);
            if (style_value)
                presentation_attribute_style.append({ .property_id = *property_id, .value = style_value.release_nonnull() });
        }
    });
    m_presentation_attribute_style = move(presentation_attribute_style);
    properties.extend(*m_presentation_attribute_style);
}

RefPtr<CSS::StyleValue const> SVGElement::parse_presentation_attribute(CSS::PropertyID property_id, Utf16View value) const
{
    CSS::Parser::ParsingParams parsing_context { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    // NB: <path>'s `d` presentational attribute is a special case - the attribute and the CSS properties
    //     syntaxes differ with the attribute being a raw path string but the CSS property only accepting a
    //     path() function. To account for this we wrap the attribute value in a path function before parsing.
    if (property_id == CSS::PropertyID::D) {
        return parse_css_value(parsing_context, Utf16String::formatted("path({})", CSS::serialize_a_string(value)), property_id);
    }

    // NB: The transform, gradientTransform, and patternTransform attributes use the SVG
    //     transform-list grammar (optional commas, unitless values, three-argument
    //     rotate), which the CSS transform property doesn't accept - so parse the SVG
    //     grammar and re-express the resulting matrix as a single-function transform
    //     list, the shape the CSS parser produces for the transform property.
    if (property_id == CSS::PropertyID::Transform) {
        auto transform_list = parse_transform(Utf16String::from_utf16(value));
        if (!transform_list.has_value())
            return {};
        auto matrix = transform_from_transform_list(*transform_list);
        CSS::StyleValueVector matrix_parameters {
            CSS::NumberStyleValue::create(matrix.a()),
            CSS::NumberStyleValue::create(matrix.b()),
            CSS::NumberStyleValue::create(matrix.c()),
            CSS::NumberStyleValue::create(matrix.d()),
            CSS::NumberStyleValue::create(matrix.e()),
            CSS::NumberStyleValue::create(matrix.f()),
        };
        return CSS::StyleValueList::create(
            CSS::StyleValueVector { CSS::TransformationStyleValue::create(CSS::PropertyID::Transform, CSS::TransformFunction::Matrix, move(matrix_parameters)) },
            CSS::StyleValueList::Separator::Space);
    }

    return parse_css_value(parsing_context, value, property_id);
}

void SVGElement::update_presentation_attribute_style(Utf16FlyString const& name, Optional<Utf16FlyString> const& namespace_)
{
    if (!m_presentation_attribute_style.has_value() || namespace_.has_value())
        return;

    auto property_id = property_id_for_presentational_attribute(name, local_name());
    if (!property_id.has_value())
        return;

    m_presentation_attribute_style->remove_all_matching([&](auto const& property) {
        return property.property_id == property_id;
    });

    for_each_attribute([&](DOM::QualifiedName const& attribute_name, Utf16View const& attribute_value) {
        if (attribute_name.namespace_().has_value())
            return;
        if (property_id_for_presentational_attribute(attribute_name.as_string(), local_name()) != property_id)
            return;
        if (auto style_value = parse_presentation_attribute(*property_id, attribute_value); style_value)
            m_presentation_attribute_style->append({ .property_id = *property_id, .value = style_value.release_nonnull() });
    });

    publish_presentation_attribute_style();
}

void SVGElement::publish_presentation_attribute_style()
{
    document().flush_deferred_style_change_event();

    Vector<CSS::StyleProperty> properties;
    Base::apply_presentational_hints(properties);
    properties.extend(*m_presentation_attribute_style);

    HashTable<CSS::PropertyID> seen_properties;
    for (size_t i = properties.size(); i > 0; --i) {
        if (seen_properties.set(properties[i - 1].property_id) != AK::HashSetResult::InsertedNewEntry)
            properties.remove(i - 1);
    }

    if (presentational_hint_properties_need_publication(properties)
        && CSS::record_element_presentational_hint_properties(*this, properties))
        did_publish_presentational_hint_properties(properties);
}

bool SVGElement::should_include_in_accessibility_tree() const
{
    bool has_title_or_desc = false;
    auto role = role_from_role_attribute_value();
    for_each_child_of_type<SVGElement>([&has_title_or_desc](auto& child) {
        if ((is<SVGTitleElement>(child) || is<SVGDescElement>(child)) && !child.text_content()->utf16_view().trim_ascii_whitespace().is_empty()) {
            has_title_or_desc = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    // https://w3c.github.io/svg-aam/#include_elements
    // TODO: Add support for the SVG tabindex attribute, and include a check for it here.
    return has_title_or_desc
        || (aria_label().has_value() && !aria_label()->utf16_view().trim_ascii_whitespace().is_empty())
        || (aria_labelled_by().has_value() && !aria_labelled_by()->utf16_view().trim_ascii_whitespace().is_empty())
        || (aria_described_by().has_value() && !aria_described_by()->utf16_view().trim_ascii_whitespace().is_empty())
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
    visitor.visit(m_class_name_animated_string);
    visitor.visit(m_reflected_attribute_cache);
}

void SVGElement::attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);
    HTMLOrSVGOrMathMLElement::attribute_changed(local_name, old_value, value, namespace_);

    if (old_value != value)
        update_presentation_attribute_style(local_name, namespace_);
    update_use_elements_that_reference_this();
}

void SVGElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);
    m_presentation_attribute_style.clear();
}

WebIDL::ExceptionOr<void> SVGElement::cloned(DOM::Node& copy, bool clone_children) const
{
    TRY(Base::cloned(copy, clone_children));
    TRY(HTMLOrSVGOrMathMLElement::cloned(copy, clone_children));
    return {};
}

void SVGElement::inserted()
{
    Base::inserted();
    HTMLOrSVGOrMathMLElement::inserted();

    update_use_elements_that_reference_this();
}

void SVGElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);

    update_use_elements_that_reference_this();
}

void SVGElement::update_use_elements_that_reference_this()
{
    if (is<SVGUseElement>(this)
        // If this element is in a shadow root, it already represents a clone and is not itself referenced.
        || is<DOM::ShadowRoot>(this->root())
        // If this does not have an id it cannot be referenced, no point in notifying use elements.
        || !id().has_value()
        // An unconnected node cannot have valid references.
        // This also prevents searches for elements that are in the process of being constructed - as clones.
        || !this->is_connected()) {

        return;
    }

    for (auto& use_element : document().svg_use_elements()) {
        if (document().is_completely_loaded())
            use_element.svg_element_changed(*this);
        else
            use_element.svg_element_changed_before_document_complete(*this);
    }
}

void SVGElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);

    auto is_use_element_shadow_root = [](Node& node) {
        auto* shadow_root = as_if<DOM::ShadowRoot>(node);
        return shadow_root && shadow_root->host() && is<SVGUseElement>(*shadow_root->host());
    };

    // If this element is in a shadow root hosted by a use element,
    // it already represents a clone and is not itself referenced.
    // NB: We check both old_root (for when the element is removed from the shadow tree directly)
    //     and root() (for when the use element host is removed from the document).
    if (is_use_element_shadow_root(old_root) || is_use_element_shadow_root(root()))
        return;

    remove_from_use_element_that_reference_this();

    // A <mask>, <clipPath>, or <pattern> referenced via url(#id) is laid out as a resource box attached to the
    // referencing element's layout subtree. So it outlives this element's own DOM node. Rebuild those referencing
    // subtrees while this element is still alive — so the stale resource boxes are dropped before this node is
    // collected.
    mark_resource_box_referencing_elements_for_layout_tree_update();

    // References that resolve during display list recording (e.g. gradient or filter url(#id) references) don't
    // build resource boxes, so there may be no layout invalidation to trigger re-recording. Request it explicitly
    // to drop the visual effects of the removed element.
    if (id().has_value())
        document().set_needs_repaint(Badge<SVGElement> {}, InvalidateDisplayList::Yes);
}

void SVGElement::register_resource_box_referencing_element(Badge<Layout::LayoutTreeBuilderAccess>, DOM::Element& referencing_element)
{
    m_resource_box_referencing_elements.remove_all_matching([&](auto& weak_element) {
        return !weak_element || weak_element == &referencing_element;
    });
    m_resource_box_referencing_elements.append(referencing_element);
}

void SVGElement::mark_resource_box_referencing_elements_for_layout_tree_update()
{
    for (auto& weak_referencing_element : m_resource_box_referencing_elements) {
        if (auto referencing_element = weak_referencing_element.ptr())
            referencing_element->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::SVGResourceElementRemoved);
    }
    m_resource_box_referencing_elements.clear();
}

void SVGElement::remove_from_use_element_that_reference_this()
{
    if (is<SVGUseElement>(this) || !id().has_value()) {
        return;
    }

    for (auto& use_element : document().svg_use_elements()) {
        // Use elements that are part of the same subtree removal may still be registered, since removal hooks run
        // after the subtree has been detached. They are no longer reachable from the document and should not be
        // notified. NB: This must check the tree structure, not Node::is_connected(), because the connected flag of
        // nodes later in tree order has not been updated yet when we get here.
        if (!use_element.root().is_document())
            continue;
        use_element.svg_element_removed(*this);
    }
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGElement__classNames
GC::Ref<SVGAnimatedString> SVGElement::class_name()
{
    // The className IDL attribute reflects the ‘class’ attribute.
    if (!m_class_name_animated_string)
        m_class_name_animated_string = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::class_, OptionalNone {}, OptionalNone {} });

    return *m_class_name_animated_string;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGElement__ownerSVGElement
GC::Ptr<SVGSVGElement> SVGElement::owner_svg_element()
{
    // The ownerSVGElement IDL attribute represents the nearest ancestor ‘svg’ element.
    // On getting ownerSVGElement, the nearest ancestor ‘svg’ element is returned;
    // if the current element is the outermost svg element, then null is returned.
    return first_flat_tree_ancestor_of_type<SVGSVGElement>();
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGElement__viewportElement
GC::Ptr<SVGElement> SVGElement::viewport_element()
{
    for (auto* node = parent(); node; node = node->parent_or_shadow_host()) {
        // https://svgwg.org/svg2-draft/coords.html#EstablishingANewSVGViewport
        // The following elements establish new SVG viewports:
        // - The ‘svg’ element
        // - A ‘symbol’ element that is instanced by a ‘use’ element.
        if (is<SVGSVGElement>(*node) || is<SVGSymbolElement>(*node)) {
            return static_cast<SVGElement*>(node);
        }
    }
    return nullptr;
}

Gfx::Size<double> SVGElement::viewport_size_for_percentage_resolution()
{
    auto viewport_size_from_layout = [&](SVGSVGElement const& viewport_element) -> Gfx::Size<double> {
        // NB: A disconnected element may have stale layout objects from before it was removed.
        if (!viewport_element.is_connected())
            return {};

        auto const* layout_node = viewport_element.layout_node();
        if (layout_node && Painting::has_committed_box(*layout_node) && Painting::is_svg_svg_paintable(*layout_node))
            return Painting::svg_viewport_size(*layout_node).to_type<double>();

        return {};
    };

    // NB: Percentages for SVGSVGElements are resolved irrespective of it's own viewBox (which only affects the internal
    //     coordinate system)
    if (auto* svg_element = as_if<SVGSVGElement>(*this)) {
        auto const* parent = svg_element->parent_or_shadow_host_element();

        // https://w3c.github.io/svgwg/svg2-draft/coords.html#InitialViewport
        if (!parent || !is<SVGElement>(*parent))
            return viewport_size_from_layout(*svg_element);

        // https://w3c.github.io/svgwg/svg2-draft/coords.html#EstablishingANewSVGViewport
        if (is<SVGForeignObjectElement>(*parent))
            return viewport_size_from_layout(*svg_element);
    }

    auto const& viewport_element = const_cast<SVGElement*>(this)->owner_svg_element().ptr();

    if (!viewport_element)
        return {};

    if (auto view_box = viewport_element->active_view_box(); view_box.has_value() && view_box->width > 0 && view_box->height > 0)
        return { view_box->width, view_box->height };

    return viewport_size_from_layout(*viewport_element);
}

GC::Ref<SVGAnimatedLength> SVGElement::svg_animated_length_for_attribute(Utf16FlyString const& attribute_name, SVGLength::Directionality directionality, SVGLengthValue default_value)
{
    if (auto cached = m_reflected_attribute_cache.get(attribute_name); cached.has_value())
        return as<SVGAnimatedLength>(*cached.value());

    auto base_length = SVGLength::create_reflected_attribute(document().relevant_settings_object().realm(), *this, attribute_name, directionality, SVGLength::ReflectedAttributeType::BaseValue, default_value, SVGLength::ReadOnly::No);
    // AD-HOC: The spec says this should reflect the base value of the attribute but other browsers reflect the SMIL
    //         animated value instead.
    auto anim_length = SVGLength::create_reflected_attribute(document().relevant_settings_object().realm(), *this, attribute_name, directionality, SVGLength::ReflectedAttributeType::AnimatedValue, default_value, SVGLength::ReadOnly::Yes);

    auto animated_length = SVGAnimatedLength::create(base_length, anim_length);
    m_reflected_attribute_cache.set(attribute_name, animated_length);
    return animated_length;
}

}
