/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLMapElement);

HTMLMapElement::HTMLMapElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMapElement::~HTMLMapElement() = default;

void HTMLMapElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_areas);
}

void HTMLMapElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // NB: A map is named by its name or its id, so either can change which images are associated with it.
    if (name.is_one_of(HTML::AttributeNames::name, HTML::AttributeNames::id))
        document().set_image_map_areas_need_publication();
}

void HTMLMapElement::inserted()
{
    Base::inserted();
    document().set_image_map_areas_need_publication();
}

void HTMLMapElement::removed_from(IsSubtreeRoot is_subtree_root, DOM::Node* old_ancestor, DOM::Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    document().set_image_map_areas_need_publication();
}

// https://html.spec.whatwg.org/multipage/interaction.html#get-the-focusable-area
GC::Ptr<HTMLImageElement> HTMLMapElement::first_image_with_focusable_shapes() const
{
    // Return the shape corresponding to the first img element in tree order that uses the image map to which the area
    // element belongs.
    return first_associated_image_matching([](HTMLImageElement& image_element) {
        // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
        // The shapes of area elements in an image map associated with an img element that is being rendered and is
        // not inert.
        return image_element.meets_focusable_area_rendering_requirements() && !image_element.is_inert();
    });
}

GC::Ptr<HTMLImageElement> HTMLMapElement::first_painted_image_with_focusable_shapes(Layout::BegunRead const& read) const
{
    return first_associated_image_matching([&read](HTMLImageElement& image_element) {
        auto const* layout_node = image_element.layout_node(read);
        return layout_node && Painting::has_committed_box(*layout_node) && !image_element.is_inert();
    });
}

// https://html.spec.whatwg.org/multipage/image-maps.html#dom-map-areas
GC::Ref<DOM::HTMLCollection> HTMLMapElement::areas()
{
    // The areas attribute must return an HTMLCollection rooted at the map element, whose filter matches only area elements.
    if (!m_areas) {
        m_areas = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [](Element const& element) { return is<HTML::HTMLAreaElement>(element); }, DOM::HTMLCollection::AttributeInvalidationType::None);
    }
    return *m_areas;
}

}
