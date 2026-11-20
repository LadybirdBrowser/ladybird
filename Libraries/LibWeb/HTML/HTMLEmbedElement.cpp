/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoading.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Checks.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLEmbedElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLEmbedElement);

HTMLEmbedElement::HTMLEmbedElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : NavigableContainer(document, move(qualified_name))
{
}

HTMLEmbedElement::~HTMLEmbedElement() = default;

void HTMLEmbedElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_image_request);
    visitor.visit(m_fetch_controller);
    visitor.visit(m_document_observer);
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-embed-element-setup-steps
// Whenever an embed element that was not potentially active becomes potentially active, and whenever a potentially
// active embed element that is remaining potentially active has its src attribute set, changed, or removed or its type
// attribute set, changed, or removed, the user agent must queue an element task on the embed task source given the
// element to run the embed element setup steps for that element.
// NB: Whether the element is potentially active depends on its layout, so these hooks queue the setup steps whenever it
//     may have become potentially active, and the setup steps check that it is (see is_potentially_active()):
//     - initialize_element() and adopted_from(): Its node document becomes fully active, or stops being fully active.
//     - post_connection() and removed_from(): It's inserted into a document, or removed from one.
//     - attribute_changed(): Its src or type attribute is set, changed, or removed.
//     - rendering_changed(): It starts or stops being rendered.
//     - ancestor_object_started_showing_fallback_content(): An ancestor object element starts showing its fallback
//       content.
void HTMLEmbedElement::initialize_element()
{
    m_document_observer = DOM::DocumentObserver::create(document());
    m_document_observer->set_document_became_active(GC::create_function(heap(), GC::captures(*this), [](auto& self) {
        self.queue_setup_steps();
    }));
    m_document_observer->set_document_became_inactive(GC::create_function(heap(), GC::captures(*this), [](auto& self) {
        self.stop_loading();
    }));
}

void HTMLEmbedElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);
    m_document_observer->retarget_for_adoption(document());
}

void HTMLEmbedElement::post_connection()
{
    Base::post_connection();
    queue_setup_steps();
}

void HTMLEmbedElement::removed_from(IsSubtreeRoot is_subtree_root, DOM::Node* old_ancestor, DOM::Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    stop_loading();
}

void HTMLEmbedElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (!namespace_.has_value() && first_is_one_of(name, AttributeNames::src, AttributeNames::type))
        queue_setup_steps();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#display-no-plugin
// Whenever an embed element that was potentially active stops being potentially active, any plugin that had been
// instantiated for that element must be unloaded.
// NB: This displays no plugin for the element, and stops whatever it was loading or showing: Its fetch, its image, and
//     its content navigable.
void HTMLEmbedElement::stop_loading()
{
    ++m_setup_generation;
    if (m_fetch_controller)
        m_fetch_controller->abort(relevant_realm(*this), {});
    m_fetch_controller = nullptr;
    m_load_event_delayer.clear();
    if (!m_image_request && !m_content_navigable)
        return;

    // NB: An image that is still being decoded from the response body delays the load event until it ends, so end it.
    if (m_image_request)
        m_image_request->response_body_fetch_was_stopped();
    m_image_request = nullptr;

    // 1. Destroy a child navigable given element.
    destroy_the_child_navigable();

    // FIXME: 2. Display an indication that no plugin could be found for element, as the contents of element.

    // 3. element now represents nothing.
    m_natural_size = {};
    update_representation();
}

CSS::ElementBoxKind HTMLEmbedElement::box_kind() const
{
    if (content_navigable())
        return CSS::ElementBoxKind::NavigableContainerViewport;
    if (m_image_request && m_image_request->image_data())
        return CSS::ElementBoxKind::Image;
    return Base::box_kind();
}

GC::Ptr<DecodedImageData> HTMLEmbedElement::decoded_image_data() const
{
    return m_image_request ? m_image_request->image_data() : nullptr;
}

void HTMLEmbedElement::set_natural_size_of_content_document(CSS::SizeWithAspectRatio const& size)
{
    m_natural_size = size;
    CSS::record_element_replaced_content_input(*this);
    set_needs_layout_update(DOM::SetNeedsLayoutReason::EmbeddedDocumentResized);
}

void HTMLEmbedElement::update_representation()
{
    CSS::record_element_box_kind(*this);
    CSS::record_element_replaced_content_input(*this);
    // NB: The parent is a shadow root for an embed at the top of a shadow tree, so it's not always an element.
    if (auto parent = this->parent())
        parent->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::HTMLEmbedElementRepresentationChanged);
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-embed-element-setup-steps
void HTMLEmbedElement::queue_setup_steps()
{
    if (!is_connected() || !document().is_fully_active())
        return;
    auto generation = ++m_setup_generation;

    // The embed element potentially delays the load event.
    // NB: The delay starts when the setup steps are queued, so that the load event can't fire between now and the fetch
    //     those steps start, and it ends once the steps have nothing more to load.
    m_load_event_delayer.emplace(document());
    queue_an_element_task(Task::Source::Embed, GC::create_function(heap(), GC::captures(*this, generation), [](auto& self, auto generation) {
        // 1. If another task has since been queued to run the embed element setup steps for element, then return.
        if (self.m_setup_generation != generation)
            return;
        self.run_setup_steps(generation);
    }),
        GC::create_function(heap(), GC::captures(*this, generation), [](auto& self, auto generation) {
            if (self.m_setup_generation == generation)
                self.m_load_event_delayer.clear();
        }));
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#concept-embed-active
bool HTMLEmbedElement::is_potentially_active()
{
    // An embed element is said to be potentially active when the following conditions are all met simultaneously:
    // - The element is in a document or was in a document the last time the event loop reached step 1.
    // AD-HOC: "In a document" is a legacy term, and the spec's use of it hasn't been updated for shadow trees:
    //         https://dom.spec.whatwg.org/#in-a-document
    //         So an embed in a shadow tree counts once it's connected, the same as one in the document tree.
    // - The element's node document is fully active.
    if (!is_connected() || !document().is_fully_active())
        return false;

    // - The element has either a src attribute set or a type attribute set (or both).
    auto src = get_attribute(AttributeNames::src);
    if (!src.has_value() && !has_attribute(AttributeNames::type))
        return false;

    // - The element's src attribute is either absent or its value is not the empty string.
    if (src.has_value() && src->is_empty())
        return false;

    // - The element is being rendered, or was being rendered the last time the event loop reached step 1.
    // NB: This is checked ahead of the ancestor conditions, so that the rendering state that rendering_changed()
    //     compares against is current, even when an ancestor keeps the element from being potentially active.
    document().update_layout(DOM::UpdateLayoutReason::HTMLEmbedElement);
    m_was_rendered = has_layout_box();
    if (!m_was_rendered)
        return false;

    // - The element is not a descendant of a media element.
    // - The element is not a descendant of an object element that is not showing its fallback content.
    for (auto ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (is<HTMLMediaElement>(*ancestor) || (is<HTMLObjectElement>(*ancestor) && !as<HTMLObjectElement>(*ancestor).shows_fallback_content()))
            return false;
    }
    return true;
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-embed-element-setup-steps
void HTMLEmbedElement::run_setup_steps(u64 generation)
{
    // NB: Step 1 is done by the task that runs these steps. And since the steps are queued whenever the element may
    //     have become potentially active, they check here that it is, and unload what it shows if it isn't.
    if (!is_potentially_active()) {
        stop_loading();
        return;
    }

    // 2. If element has a src attribute set:
    auto src = get_attribute(AttributeNames::src);
    if (!src.has_value()) {
        // 3. Otherwise, display no plugin for element.
        stop_loading();
        return;
    }

    // 1. Let url be the result of encoding-parsing a URL given element's src attribute's value, relative to element's
    //    node document.
    auto url = document().encoding_parse_url(*src);

    // 2. If url is failure, then return.
    // AD-HOC: The spec leaves whatever a previous src loaded in place. Instead, display no plugin, as the element no
    //         longer names anything to show.
    if (!url.has_value()) {
        stop_loading();
        return;
    }

    // 3. Let request be a new request whose URL is url, client is element's node document's relevant settings object,
    //    destination is "embed", credentials mode is "include", mode is "navigate", initiator type is "embed", and
    //    whose use-URL-credentials flag is set.
    using Request = Fetch::Infrastructure::Request;
    auto request = Request::create();
    request->set_url(url.release_value());
    request->set_client(document().relevant_settings_object().address());
    request->set_destination(Request::Destination::Embed);
    request->set_credentials_mode(Request::CredentialsMode::Include);
    request->set_mode(Request::Mode::Navigate);
    request->set_initiator_type(Request::InitiatorType::Embed);
    request->set_use_url_credentials(true);

    // 4. Fetch request, with processResponse set to the following steps given response response:
    // NB: A fetch from an earlier run of these steps is no longer wanted, so abort it.
    if (m_fetch_controller)
        m_fetch_controller->abort(relevant_realm(*this), {});
    Fetch::Infrastructure::FetchAlgorithms::Input algorithms { Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent };
    algorithms.process_response = GC::create_function(heap(), GC::captures(*this, generation), [](auto& self, auto generation, GC::Ref<Fetch::Infrastructure::ResponseView> response) {
        // 1. If another task has since been queued to run the embed element setup steps for element, then return.
        if (generation == self.m_setup_generation)
            self.handle_response(response, generation);
    });
    m_fetch_controller = Fetch::Fetching::fetch(relevant_realm(*this), request, Fetch::Infrastructure::FetchAlgorithms::create(move(algorithms)));

    // Fetching the resource must delay the load event of element's node document.
    // NB: The delay that queue_setup_steps() started lasts until the response has been handled.

    // AD-HOC: A stopped fetch, such as one of a document that was stopped, never processes its response. Stop delaying
    //         the load event on it then, as if the user agent had given up on loading it.
    m_fetch_controller->set_stop_steps(GC::create_function(heap(), GC::captures(GC::Weak { *this }, generation), [](auto& weak_this, auto generation) {
        auto embed = weak_this.ptr();
        if (!embed || embed->m_setup_generation != generation)
            return;
        embed->m_fetch_controller = nullptr;
        embed->m_load_event_delayer.clear();

        // NB: An image that is being decoded from the response body won't get the rest of it either.
        if (auto image_request = embed->m_image_request)
            image_request->response_body_fetch_was_stopped();
    }));
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-embed-element-setup-steps
// NB: These are the processResponse steps of step 2.4 of the embed element setup steps, from step 2 on.
void HTMLEmbedElement::handle_response(Fetch::Infrastructure::ResponseView& response, u64 generation)
{
    // 2. If response is a network error, then fire an event named load at element, and return.
    // AD-HOC: The spec leaves whatever a previous src loaded in place. Instead, display no plugin, as nothing loaded.
    auto handle_network_error = [&] {
        stop_loading();
        dispatch_event(DOM::Event::create(EventNames::load, HighResolutionTime::current_high_resolution_time(relevant_global_object(*this))));
    };
    // NB: A response without a URL has nothing to navigate to, so it's handled as a network error.
    if (response.is_network_error() || !response.url().has_value()) {
        handle_network_error();
        return;
    }

    // 3. Let type be the result of determining the type of content given element and response.
    // AD-HOC: The spec determines the type of the content only for a plugin, and otherwise returns null, so that the
    //         element displays no plugin for an HTML or SVG document, or for an image. Instead, take the type from the
    //         response's Content-Type, or from a type attribute that names an image type or a type of document that
    //         can be loaded, and show documents and images, as the web expects:
    //         https://github.com/whatwg/html/issues/513
    auto type = Fetch::Infrastructure::extract_mime_type(response.header_list());
    if (auto declared = get_attribute(AttributeNames::type); declared.has_value()) {
        if (auto mime = MimeSniff::MimeType::parse(*declared); mime.has_value() && (mime->is_image() || can_load_document_with_type(*mime)))
            type = move(mime);
    }

    // 4. Switch on type:
    //    null
    //        1. Display no plugin for element.
    if (!type.has_value()) {
        stop_loading();
        return;
    }

    // AD-HOC: The spec has no case for an image. Rather than navigating the content navigable to an image document,
    //         show a raster image as the element's own image:
    //         https://github.com/whatwg/html/issues/7939
    if (type->is_image() && !type->is_xml() && response.body()) {
        // NB: The setup fetch uses navigation mode, but the raster response is consumed as an image subresource, so it
        //     gets the cross-origin resource policy check of one.
        // https://fetch.spec.whatwg.org/#cross-origin-resource-policy-check
        if (Fetch::Infrastructure::is_http_or_https_scheme(response.url()->scheme())
            && Fetch::Fetching::cross_origin_resource_policy_check(document().origin(), document().policy_container()->embedder_policy, *response.record()) == Fetch::Infrastructure::RequestOrResponseBlocking::Blocked) {
            handle_network_error();
            return;
        }
        destroy_the_child_navigable();
        m_natural_size = {};
        m_image_request = SharedResourceRequest::create_from_image_response_body(document(), *response.url(), *response.body(), !document().origin().is_same_origin(response.url()->origin()), {});
        auto complete = GC::create_function(heap(), GC::captures(*this, generation), [](auto& self, auto generation) {
            if (generation != self.m_setup_generation)
                return;
            self.update_representation();
            self.m_load_event_delayer.clear();
            self.m_fetch_controller = nullptr;
            self.dispatch_event(DOM::Event::create(EventNames::load, HighResolutionTime::current_high_resolution_time(relevant_global_object(self))));
        });
        auto discard = GC::create_function(heap(), GC::captures(*this, generation), [](auto& self, auto generation) {
            if (generation == self.m_setup_generation)
                self.stop_loading();
        });
        m_image_request->add_callbacks(complete, discard, discard);
        update_representation();
        return;
    }

    // NB: A type that names neither an image nor a type of document that can be loaded is null, as above.
    if (!can_load_document_with_type(*type)) {
        stop_loading();
        return;
    }

    // AD-HOC: The spec does not stop an embed from embedding the document that contains it, so the nesting never ends
    //         and the load event never fires. As for an object element, allow one level of self-reference, and display
    //         no plugin at the second.
    size_t matching_ancestors = 0;
    for (auto const& ancestor : document().inclusive_ancestor_navigables()) {
        auto ancestor_url = ancestor->active_document_url();
        if (ancestor_url.has_value() && ancestor_url->equals(*response.url(), URL::ExcludeFragment::Yes) && ++matching_ancestors >= 2) {
            stop_loading();
            return;
        }
    }

    //    Otherwise
    m_image_request = nullptr;
    m_natural_size = {};

    //        1. If element's content navigable is null, then create a new child navigable for element.
    if (!m_content_navigable)
        create_new_child_navigable();

    //        2. Navigate element's content navigable to response's URL using element's node document, with response set
    //           to response, and historyHandling set to "replace".
    // FIXME: Navigate with response set to response, rather than fetching the document a second time.
    if (m_content_navigable) {
        MUST(m_content_navigable->navigate({
            .url = *response.url(),
            .source_document = document(),
            .history_handling = NavigationHistoryBehavior::Replace,
        }));
    }

    // NB: The content navigable delays the load event while it loads the document, so the element no longer needs to.
    //     And the navigation fetches the document itself, so this fetch can stop.
    m_load_event_delayer.clear();
    if (m_fetch_controller)
        m_fetch_controller->abort(relevant_realm(*this), {});
    m_fetch_controller = nullptr;

    //        3. element now represents its content navigable.
    update_representation();
}

void HTMLEmbedElement::rendering_changed()
{
    if (!has_attribute(AttributeNames::src) || !is_connected())
        return;
    queue_an_element_task(Task::Source::Embed, GC::create_function(heap(), GC::captures(*this), [](auto& self) {
        if (self.has_layout_box() != self.m_was_rendered)
            self.queue_setup_steps();
    }));
}

void HTMLEmbedElement::ancestor_object_started_showing_fallback_content()
{
    // NB: The embed may start being rendered now, but it gets its layout box only at the next layout update, which can
    //     come after the object's fetch stops delaying the document's load event. Queueing the setup steps right away
    //     delays that load event until the embed has loaded.
    if (has_attribute(AttributeNames::src))
        queue_setup_steps();
}

bool HTMLEmbedElement::is_presentational_hint(Utf16FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::align,
        HTML::AttributeNames::height,
        HTML::AttributeNames::hspace,
        HTML::AttributeNames::vspace,
        HTML::AttributeNames::width);
}

void HTMLEmbedElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);
    for_each_attribute([&](Utf16FlyString const& name, Utf16View value) {
        if (name == HTML::AttributeNames::align) {
            // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images
            // When an embed, iframe, img, or object element, or an input element whose type attribute is in the Image Button state,
            // has an align attribute whose value is an ASCII case-insensitive match for the string "center" or the string "middle",
            // the user agent is expected to act as if the element's 'vertical-align' property was set to a value that aligns the
            // vertical middle of the element with the parent element's baseline.
            if (value.equals_ignoring_ascii_case(u"center"sv) || value.equals_ignoring_ascii_case(u"middle"sv))
                properties.append({ .property_id = CSS::PropertyID::VerticalAlign, .value = CSS::KeywordStyleValue::create(CSS::Keyword::LibwebBaselineMiddle) });
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value))
                properties.append({ .property_id = CSS::PropertyID::Height, .value = *parsed_value });
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:maps-to-the-dimension-property
        else if (name == HTML::AttributeNames::hspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                properties.append({ .property_id = CSS::PropertyID::MarginLeft, .value = *parsed_value });
                properties.append({ .property_id = CSS::PropertyID::MarginRight, .value = *parsed_value });
            }
        } else if (name == HTML::AttributeNames::vspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                properties.append({ .property_id = CSS::PropertyID::MarginTop, .value = *parsed_value });
                properties.append({ .property_id = CSS::PropertyID::MarginBottom, .value = *parsed_value });
            }
        } else if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                properties.append({ .property_id = CSS::PropertyID::Width, .value = *parsed_value });
            }
        }
    });
}

}
