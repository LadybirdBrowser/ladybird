/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLIFrameElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Origin.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Layout/FrameBox.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLIFrameElement);

HTMLIFrameElement::HTMLIFrameElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : NavigableContainer(document, move(qualified_name))
{
}

HTMLIFrameElement::~HTMLIFrameElement() = default;

void HTMLIFrameElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLIFrameElement);
}

JS::GCPtr<Layout::Node> HTMLIFrameElement::create_layout_node(NonnullRefPtr<CSS::StyleProperties> style)
{
    return heap().allocate_without_realm<Layout::FrameBox>(document(), *this, move(style));
}

void HTMLIFrameElement::attribute_changed(FlyString const& name, Optional<String> const& value)
{
    HTMLElement::attribute_changed(name, value);

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:process-the-iframe-attributes-2
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:process-the-iframe-attributes-3
    // Whenever an iframe element with a non-null content navigable has its srcdoc attribute set, changed, or removed,
    // the user agent must process the iframe attributes.
    // Similarly, whenever an iframe element with a non-null content navigable but with no srcdoc attribute specified
    // has its src attribute set, changed, or removed, the user agent must process the iframe attributes.
    if (m_content_navigable) {
        if (name == AttributeNames::srcdoc || (name == AttributeNames::src && !has_attribute(AttributeNames::srcdoc)))
            process_the_iframe_attributes();
    }
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:the-iframe-element-6
void HTMLIFrameElement::inserted()
{
    HTMLElement::inserted();

    // When an iframe element element is inserted into a document whose browsing context is non-null, the user agent must run these steps:
    if (in_a_document_tree() && document().browsing_context()) {
        // 1. Create a new child navigable for element.
        MUST(create_new_child_navigable([this] {
            // 3. Process the iframe attributes for element, with initialInsertion set to true.
            process_the_iframe_attributes(true);
            set_content_navigable_initialized();
        }));

        // FIXME: 2. If element has a sandbox attribute, then parse the sandboxing directive given the attribute's value and element's iframe sandboxing flag set.
    }
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#process-the-iframe-attributes
void HTMLIFrameElement::process_the_iframe_attributes(bool initial_insertion)
{
    if (!content_navigable())
        return;

    // Make sure applying of history step caused by potential sync navigation to "about:blank"
    // is finished. Otherwise, it might interrupt navigation caused by changing src or srcdoc.
    if (!initial_insertion && !content_navigable_initialized()) {
        main_thread_event_loop().spin_processing_tasks_with_source_until(Task::Source::NavigationAndTraversal, [this] { return content_navigable_initialized(); });
    }

    // 1. If element's srcdoc attribute is specified, then:
    if (has_attribute(HTML::AttributeNames::srcdoc)) {
        // 1. Set element's current navigation was lazy loaded boolean to false.
        set_current_navigation_was_lazy_loaded(false);

        // 2. If the will lazy load element steps given element return true, then:
        if (will_lazy_load_element()) {
            // 1. Set element's lazy load resumption steps to the rest of this algorithm starting with the step labeled navigate to the srcdoc resource.
            set_lazy_load_resumption_steps([this]() {
                // 3. Navigate to the srcdoc resource: navigate an iframe or frame given element, about:srcdoc, the empty string, and the value of element's srcdoc attribute.
                navigate_an_iframe_or_frame(URL::URL("about:srcdoc"sv), ReferrerPolicy::ReferrerPolicy::EmptyString, get_attribute(HTML::AttributeNames::srcdoc));

                // FIXME: The resulting Document must be considered an iframe srcdoc document.
            });

            // 2. Set element's current navigation was lazy loaded boolean to true.
            set_current_navigation_was_lazy_loaded(true);

            // 3. Start intersection-observing a lazy loading element for element.
            document().start_intersection_observing_a_lazy_loading_element(*this);

            // 4. Return.
            return;
        }

        // 3. Navigate to the srcdoc resource: navigate an iframe or frame given element, about:srcdoc, the empty string, and the value of element's srcdoc attribute.
        navigate_an_iframe_or_frame(URL::URL("about:srcdoc"sv), ReferrerPolicy::ReferrerPolicy::EmptyString, get_attribute(HTML::AttributeNames::srcdoc));

        // FIXME: The resulting Document must be considered an iframe srcdoc document.

        return;
    }

    // 1. Let url be the result of running the shared attribute processing steps for iframe and frame elements given element and initialInsertion.
    auto url = shared_attribute_processing_steps_for_iframe_and_frame(initial_insertion);

    // 2. If url is null, then return.
    if (!url.has_value()) {
        return;
    }

    // 3. If url matches about:blank and initialInsertion is true, then:
    if (url_matches_about_blank(*url) && initial_insertion) {
        // 1. Run the iframe load event steps given element.
        run_iframe_load_event_steps(*this);

        // 2. Return.
        return;
    }

    // 4. Let referrerPolicy be the current state of element's referrerpolicy content attribute.
    auto referrer_policy = ReferrerPolicy::from_string(get_attribute_value(HTML::AttributeNames::referrerpolicy)).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);

    // 5. Set element's current navigation was lazy loaded boolean to false.
    set_current_navigation_was_lazy_loaded(false);

    // 6. If the will lazy load element steps given element return true, then:
    if (will_lazy_load_element()) {
        // 1. Set element's lazy load resumption steps to the rest of this algorithm starting with the step labeled navigate.
        set_lazy_load_resumption_steps([this, url, referrer_policy]() {
            // 7. Navigate: navigate an iframe or frame given element, url, and referrerPolicy.
            navigate_an_iframe_or_frame(*url, referrer_policy);
        });

        // 2. Set element's current navigation was lazy loaded boolean to true.
        set_current_navigation_was_lazy_loaded(true);

        // 3. Start intersection-observing a lazy loading element for element.
        document().start_intersection_observing_a_lazy_loading_element(*this);

        // 4. Return.
        return;
    }

    // 7. Navigate: navigate an iframe or frame given element, url, and referrerPolicy.
    navigate_an_iframe_or_frame(*url, referrer_policy);
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:the-iframe-element-7
void HTMLIFrameElement::removed_from(DOM::Node* node)
{
    HTMLElement::removed_from(node);

    // When an iframe element is removed from a document, the user agent must destroy the nested navigable of the element.
    destroy_the_child_navigable();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#iframe-load-event-steps
void run_iframe_load_event_steps(HTML::HTMLIFrameElement& element)
{
    // FIXME: 1. Assert: element's content navigable is not null.
    if (!element.content_navigable()) {
        // FIXME: For some reason, we sometimes end up here in the middle of SunSpider.
        dbgln("FIXME: run_iframe_load_event_steps called with null nested browsing context");
        return;
    }

    // 2. Let childDocument be element's content navigable's active document.
    [[maybe_unused]] auto child_document = element.content_navigable()->active_document();

    // FIXME: 3. If childDocument has its mute iframe load flag set, then return.

    // FIXME: 4. Set childDocument's iframe load in progress flag.

    // 5. Fire an event named load at element.
    element.dispatch_event(DOM::Event::create(element.realm(), HTML::EventNames::load));

    // FIXME: 6. Unset childDocument's iframe load in progress flag.
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLIFrameElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

void HTMLIFrameElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visit_lazy_loading_element(visitor);
}

void HTMLIFrameElement::set_current_navigation_was_lazy_loaded(bool value)
{
    m_current_navigation_was_lazy_loaded = value;

    // An iframe element whose current navigation was lazy loaded boolean is false potentially delays the load event.
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:potentially-delays-the-load-event
    set_potentially_delays_the_load_event(!value);
}

}
