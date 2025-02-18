/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLFrameElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLFrameElement.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLFrameElement);

HTMLFrameElement::HTMLFrameElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : NavigableContainer(document, move(qualified_name))
{
    // https://html.spec.whatwg.org/multipage/obsolete.html#frames:potentially-delays-the-load-event
    // The frame element potentially delays the load event.
    set_potentially_delays_the_load_event(true);
}

HTMLFrameElement::~HTMLFrameElement() = default;

void HTMLFrameElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLFrameElement);
}

// https://html.spec.whatwg.org/multipage/obsolete.html#frames:html-element-insertion-steps
void HTMLFrameElement::inserted()
{
    Base::inserted();

    // 1. If insertedNode is not in a document tree, then return.
    if (!in_a_document_tree())
        return;

    // 2. If insertedNode's root's browsing context is null, then return.
    if (root().document().browsing_context() == nullptr)
        return;

    // 3. Create a new child navigable for insertedNode.
    MUST(create_new_child_navigable(GC::create_function(realm().heap(), [this] {
        // 4. Process the frame attributes for insertedNode, with initialInsertion set to true.
        process_the_frame_attributes(true);
        set_content_navigable_initialized();
    })));
}

// https://html.spec.whatwg.org/multipage/obsolete.html#frames:html-element-removing-steps
void HTMLFrameElement::removed_from(DOM::Node* old_parent, DOM::Node& old_root)
{
    Base::removed_from(old_parent, old_root);

    // The frame HTML element removing steps, given removedNode, are to destroy a child navigable given removedNode.
    destroy_the_child_navigable();
}

// https://html.spec.whatwg.org/multipage/obsolete.html#frames:frame-3
void HTMLFrameElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // Whenever a frame element with a non-null content navigable has its src attribute set, changed, or removed, the
    // user agent must process the frame attributes.
    if (content_navigable() && name == HTML::AttributeNames::src)
        process_the_frame_attributes();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLFrameElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

void HTMLFrameElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

// https://html.spec.whatwg.org/multipage/obsolete.html#process-the-frame-attributes
void HTMLFrameElement::process_the_frame_attributes(bool initial_insertion)
{
    // 1. Let url be the result of running the shared attribute processing steps for iframe and frame elements given
    //    element and initialInsertion.
    auto url = shared_attribute_processing_steps_for_iframe_and_frame(initial_insertion);

    // 2. If url is null, then return.
    if (!url.has_value())
        return;

    // 3. If url matches about:blank and initialInsertion is true, then:
    if (url_matches_about_blank(*url) && initial_insertion) {
        // 1. Fire an event named load at element.
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));

        // 2. Return.
        return;
    }

    // 3. Navigate an iframe or frame given element, url, and the empty string.
    navigate_an_iframe_or_frame(*url, ReferrerPolicy::ReferrerPolicy::EmptyString);
}

}
