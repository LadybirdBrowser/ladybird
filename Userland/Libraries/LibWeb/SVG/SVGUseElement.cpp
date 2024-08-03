/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGUseElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGUseElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGUseElement);

SVGUseElement::SVGUseElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, qualified_name)
{
}

void SVGUseElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGUseElement);

    // The shadow tree is open (inspectable by script), but read-only.
    auto shadow_root = heap().allocate<DOM::ShadowRoot>(realm, document(), *this, Bindings::ShadowRootMode::Open);

    // The user agent must create a use-element shadow tree whose host is the ‘use’ element itself
    set_shadow_root(shadow_root);

    m_document_observer = realm.heap().allocate<DOM::DocumentObserver>(realm, realm, document());
    m_document_observer->set_document_completely_loaded([this]() {
        clone_element_tree_as_our_shadow_tree(referenced_element());
    });
}

void SVGUseElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_document_observer);
    visitor.visit(m_resource_request);
}

void SVGUseElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    Base::attribute_changed(name, old_value, value);

    // https://svgwg.org/svg2-draft/struct.html#UseLayout
    if (name == SVG::AttributeNames::x) {
        m_x = AttributeParser::parse_coordinate(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::y) {
        m_y = AttributeParser::parse_coordinate(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::href || name == "xlink:href"_fly_string) {
        // When the ‘href’ attribute is set (or, in the absence of an ‘href’ attribute, an ‘xlink:href’ attribute), the user agent must process the URL.
        process_the_url(value);
    }
}

// https://www.w3.org/TR/SVG2/linking.html#processingURL
void SVGUseElement::process_the_url(Optional<String> const& href)
{
    // In all other cases, the URL is for a resource to be used in this SVG document. The user agent
    // must parse the URL to separate out the target fragment from the rest of the URL, and compare
    // it with the document base URL. If all parts other than the target fragment are equal, this is
    // a same-document URL reference, and processing the URL must continue as indicated in Identifying
    // the target element with the current document as the referenced document.
    m_href = document().url().complete_url(href.value_or(String {}));
    if (!m_href.is_valid())
        return;

    if (is_referrenced_element_same_document()) {
        clone_element_tree_as_our_shadow_tree(referenced_element());
    } else {
        fetch_the_document(m_href);
    }
}

bool SVGUseElement::is_referrenced_element_same_document() const
{
    return m_href.equals(document().url(), URL::ExcludeFragment::Yes);
}

Gfx::AffineTransform SVGUseElement::element_transform() const
{
    // The x and y properties define an additional transformation (translate(x,y), where x and y represent the computed value of the corresponding property)
    // to be applied to the ‘use’ element, after any transformations specified with other properties
    return Base::element_transform().translate(m_x.value_or(0), m_y.value_or(0));
}

void SVGUseElement::inserted()
{
    Base::inserted();
}

void SVGUseElement::svg_element_changed(SVGElement& svg_element)
{
    auto to_clone = referenced_element();
    if (!to_clone) {
        return;
    }

    // NOTE: We need to check the ancestor because attribute_changed of a child doesn't call children_changed on the parent(s)
    if (to_clone == &svg_element || to_clone->is_ancestor_of(svg_element)) {
        clone_element_tree_as_our_shadow_tree(to_clone);
    }
}

void SVGUseElement::svg_element_removed(SVGElement& svg_element)
{
    if (!m_href.fragment().has_value() || !is_referrenced_element_same_document()) {
        return;
    }

    if (AK::StringUtils::matches(svg_element.get_attribute_value("id"_fly_string), m_href.fragment().value())) {
        shadow_root()->remove_all_children();
    }
}

// https://svgwg.org/svg2-draft/linking.html#processingURL-target
JS::GCPtr<DOM::Element> SVGUseElement::referenced_element()
{
    if (!m_href.is_valid())
        return nullptr;

    if (!m_href.fragment().has_value())
        return nullptr;

    if (is_referrenced_element_same_document())
        return document().get_element_by_id(*m_href.fragment());

    if (!m_resource_request)
        return nullptr;

    auto data = m_resource_request->image_data();
    if (!data || !is<SVG::SVGDecodedImageData>(*data))
        return nullptr;

    return verify_cast<SVG::SVGDecodedImageData>(*data).svg_document().get_element_by_id(*m_href.fragment());
}

// https://svgwg.org/svg2-draft/linking.html#processingURL-fetch
void SVGUseElement::fetch_the_document(URL::URL const& url)
{
    m_load_event_delayer.emplace(document());
    m_resource_request = HTML::SharedResourceRequest::get_or_create(realm(), document().page(), url);
    m_resource_request->add_callbacks(
        [this] {
            clone_element_tree_as_our_shadow_tree(referenced_element());
            m_load_event_delayer.clear();
        },
        [this] {
            m_load_event_delayer.clear();
        });

    if (m_resource_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(vm(), url, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document().relevant_settings_object());
        m_resource_request->fetch_resource(realm(), request);
    }
}

// https://svgwg.org/svg2-draft/struct.html#UseShadowTree
void SVGUseElement::clone_element_tree_as_our_shadow_tree(Element* to_clone)
{
    shadow_root()->remove_all_children();

    if (to_clone && is_valid_reference_element(*to_clone)) {
        // The ‘use’ element references another element, a copy of which is rendered in place of the ‘use’ in the document.
        auto cloned_reference_node = MUST(to_clone->clone_node(nullptr, true));
        shadow_root()->append_child(cloned_reference_node).release_value_but_fixme_should_propagate_errors();
    }
}

bool SVGUseElement::is_valid_reference_element(Element const& reference_element) const
{
    // If the referenced element that results from resolving the URL is not an SVG element, then the reference is invalid and the ‘use’ element is in error.
    // If the referenced element is a (shadow-including) ancestor of the ‘use’ element, then this is an invalid circular reference and the ‘use’ element is in error.
    return reference_element.is_svg_element() && !reference_element.is_ancestor_of(*this);
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementXAttribute
JS::NonnullGCPtr<SVGAnimatedLength> SVGUseElement::x() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_x.value_or(0));
    auto anim_length = SVGLength::create(realm(), 0, m_x.value_or(0));
    return SVGAnimatedLength::create(realm(), move(base_length), move(anim_length));
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementYAttribute
JS::NonnullGCPtr<SVGAnimatedLength> SVGUseElement::y() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_y.value_or(0));
    auto anim_length = SVGLength::create(realm(), 0, m_y.value_or(0));
    return SVGAnimatedLength::create(realm(), move(base_length), move(anim_length));
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGUseElement::width() const
{
    // FIXME: Implement this properly.
    return SVGAnimatedLength::create(realm(), SVGLength::create(realm(), 0, 0), SVGLength::create(realm(), 0, 0));
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGUseElement::height() const
{
    // FIXME: Implement this properly.
    return SVGAnimatedLength::create(realm(), SVGLength::create(realm(), 0, 0), SVGLength::create(realm(), 0, 0));
}

// https://svgwg.org/svg2-draft/struct.html#TermInstanceRoot
JS::GCPtr<SVGElement> SVGUseElement::instance_root() const
{
    return const_cast<DOM::ShadowRoot&>(*shadow_root()).first_child_of_type<SVGElement>();
}

JS::GCPtr<SVGElement> SVGUseElement::animated_instance_root() const
{
    return instance_root();
}

JS::GCPtr<Layout::Node> SVGUseElement::create_layout_node(NonnullRefPtr<CSS::StyleProperties> style)
{
    return heap().allocate_without_realm<Layout::SVGGraphicsBox>(document(), *this, move(style));
}

}
