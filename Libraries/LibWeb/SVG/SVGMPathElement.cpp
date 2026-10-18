/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SVGAnimateMotionElement.h>
#include <LibWeb/SVG/SVGMPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGMPathElement);

SVGMPathElement::SVGMPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGMPathElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
}

void SVGMPathElement::inserted()
{
    Base::inserted();
    motion_path_changed(parent());
}

void SVGMPathElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    if (is_subtree_root == IsSubtreeRoot::Yes)
        motion_path_changed(old_ancestor);
}

void SVGMPathElement::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<Node> old_ancestor)
{
    Base::moved_from(is_subtree_root, old_ancestor);
    if (is_subtree_root == IsSubtreeRoot::Yes)
        motion_path_changed(old_ancestor);
    motion_path_changed(parent());
}

void SVGMPathElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (name == AttributeNames::href && (!namespace_.has_value() || namespace_ == Namespace::XLink))
        motion_path_changed(parent());
}

void SVGMPathElement::motion_path_changed(GC::Ptr<Node> parent)
{
    if (auto* animate_motion_element = as_if<SVGAnimateMotionElement>(parent.ptr()))
        animate_motion_element->schedule_referenced_motion_path_update();
}

// https://svgwg.org/specs/animations/#MPathElementHrefAttribute
GC::Ptr<DOM::Element> SVGMPathElement::referenced_element()
{
    // A URL reference to the ‘path’ element or shape element which defines the motion path.
    // Refer to the common handling defined for URL reference attributes and deprecated XLink attributes.
    // FIXME: Resolve references to elements in other documents.
    auto href = get_attribute_ns(OptionalNone {}, AttributeNames::href);
    if (!href.has_value())
        href = get_attribute_ns(Namespace::XLink, AttributeNames::href);
    if (!href.has_value())
        return {};
    auto url = document().encoding_parse_url(*href);
    if (!url.has_value() || !url->fragment().has_value())
        return {};
    auto* root = as_if<DOM::ParentNode>(this->root());
    if (!root)
        return {};
    return root->get_element_by_id(decode_fragment_identifier(*url->fragment()));
}

}
