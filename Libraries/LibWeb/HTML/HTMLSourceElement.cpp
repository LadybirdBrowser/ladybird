/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLSourceElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLPictureElement.h>
#include <LibWeb/HTML/HTMLSourceElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLSourceElement);

HTMLSourceElement::HTMLSourceElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLSourceElement::~HTMLSourceElement() = default;

void HTMLSourceElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLSourceElement);
    Base::initialize(realm);
}

static void update_image_children_of_picture(DOM::Node& picture)
{
    for (auto* child = picture.first_child(); child; child = child->next_sibling()) {
        if (auto* img = as_if<HTMLImageElement>(child))
            img->update_the_image_data(true);
    }
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#the-source-element:html-element-insertion-steps
void HTMLSourceElement::inserted()
{
    // The source HTML element insertion steps, given insertedNode, are:
    Base::inserted();

    // 1. Let parent be insertedNode's parent.
    auto* parent = this->parent();

    // 2. If parent is a media element that has no src attribute and whose networkState has the value NETWORK_EMPTY,
    //    then invoke that media element's resource selection algorithm.
    if (auto* media_element = as_if<HTMLMediaElement>(parent); media_element
        && !media_element->has_attribute(HTML::AttributeNames::src)
        && media_element->network_state() == HTMLMediaElement::NetworkState::Empty) {
        media_element->select_resource();
    }

    // 3. If parent is a picture element, then for each child of parent's children, if child is an img element, then
    //    count this as a relevant mutation for child.
    if (auto* picture = as_if<HTMLPictureElement>(parent))
        update_image_children_of_picture(*picture);
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#the-source-element:the-source-element-17
void HTMLSourceElement::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<DOM::Node> old_ancestor)
{
    // The source HTML element moving steps, given movedNode, isSubtreeRoot, and oldAncestor are:
    Base::moved_from(is_subtree_root, old_ancestor);

    // 1. If isSubtreeRoot is true and oldAncestor is a picture element, then for each child of oldAncestor's
    //    children: if child is an img element, then count this as a relevant mutation for child.
    if (is_subtree_root == IsSubtreeRoot::Yes) {
        if (auto* picture = as_if<HTMLPictureElement>(old_ancestor.ptr()))
            update_image_children_of_picture(*picture);
    }

    // The img element may also have moved into a (new) picture parent; the "picture's children
    // changed" mutation covers that for the new ancestor too.
    if (auto* picture = as_if<HTMLPictureElement>(parent()))
        update_image_children_of_picture(*picture);
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#the-source-element:the-source-element-18
void HTMLSourceElement::removed_from(IsSubtreeRoot is_subtree_root, DOM::Node* old_ancestor, DOM::Node& old_root)
{
    // The source HTML element removing steps, given removedNode, isSubtreeRoot, and oldAncestor are:
    Base::removed_from(is_subtree_root, old_ancestor, old_root);

    // 1. If isSubtreeRoot is true and oldAncestor is a picture element, then for each child of oldAncestor's
    //    children: if child is an img element, then count this as a relevant mutation for child.
    if (is_subtree_root == IsSubtreeRoot::Yes) {
        if (auto* picture = as_if<HTMLPictureElement>(old_ancestor))
            update_image_children_of_picture(*picture);
    }
}

// https://html.spec.whatwg.org/multipage/images.html#relevant-mutations
// "The element's parent is a picture element and a source element that is a previous sibling has
//  its srcset, sizes, media, type, width or height attributes set, changed, or removed."
void HTMLSourceElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (!name.is_one_of(
            HTML::AttributeNames::srcset,
            HTML::AttributeNames::sizes,
            HTML::AttributeNames::media,
            HTML::AttributeNames::type,
            HTML::AttributeNames::width,
            HTML::AttributeNames::height))
        return;

    if (!is<HTMLPictureElement>(parent()))
        return;

    // Only following img siblings consider this source a "previous sibling".
    for (auto* sibling = next_sibling(); sibling; sibling = sibling->next_sibling()) {
        if (auto* img = as_if<HTMLImageElement>(sibling))
            img->update_the_image_data(true);
    }
}

}
