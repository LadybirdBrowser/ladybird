/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/PixelUnits.h>

namespace Web::HTML {

class HTMLMapElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLMapElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLMapElement);

public:
    virtual ~HTMLMapElement() override;

    GC::Ref<DOM::HTMLCollection> areas();

    GC::Ptr<HTMLAreaElement> area_for_point(CSSPixelPoint, CSSPixelSize image_size);

    template<typename Callback>
    void for_each_associated_image(Callback callback) const
    {
        const_cast<DOM::Node&>(root()).for_each_in_inclusive_subtree_of_type<HTMLImageElement>([&](HTMLImageElement& image_element) {
            if (image_element.associated_map_element().ptr() != this)
                return TraversalDecision::Continue;
            return callback(image_element);
        });
    }

    GC::Ptr<HTMLImageElement> first_image_with_focusable_shapes() const;
    GC::Ptr<HTMLImageElement> first_painted_image_with_focusable_shapes() const;

private:
    HTMLMapElement(DOM::Document&, DOM::QualifiedName);

    template<typename Predicate>
    GC::Ptr<HTMLImageElement> first_associated_image_matching(Predicate predicate) const
    {
        GC::Ptr<HTMLImageElement> first_image;
        for_each_associated_image([&](HTMLImageElement& image_element) {
            if (!predicate(image_element))
                return TraversalDecision::Continue;
            first_image = image_element;
            return TraversalDecision::Break;
        });
        return first_image;
    }

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<DOM::HTMLCollection> m_areas;
};

}
