/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/HTMLObjectElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/DOM/DocumentLoading.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/MimeSniff/Resource.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLObjectElement);

HTMLObjectElement::HTMLObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : NavigableContainer(document, move(qualified_name))
{
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:potentially-delays-the-load-event
    set_potentially_delays_the_load_event(true);

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element
    // Whenever one of the following conditions occur:
    // - the element is created,
    // ...the user agent must queue an element task on the DOM manipulation task source given the object element to run
    // the following steps to (re)determine what the object element represents.
    queue_element_task_to_run_object_representation_steps();
}

HTMLObjectElement::~HTMLObjectElement() = default;

void HTMLObjectElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLObjectElement);

    m_document_observer = realm.create<DOM::DocumentObserver>(realm, document());

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element
    // Whenever one of the following conditions occur:
    // - the element's node document changes whether it is fully active,
    // ...the user agent must queue an element task on the DOM manipulation task source given the object element to run
    // the following steps to (re)determine what the object element represents.
    m_document_observer->set_document_became_active([this]() {
        queue_element_task_to_run_object_representation_steps();
    });
    m_document_observer->set_document_became_inactive([this]() {
        queue_element_task_to_run_object_representation_steps();
    });
}

void HTMLObjectElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_resource_request);
    visitor.visit(m_document_observer);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-willvalidate
bool HTMLObjectElement::will_validate()
{
    // The willValidate attribute's getter must return true, if this element is a candidate for constraint validation,
    // and false otherwise (i.e., false if any conditions are barring it from constraint validation).
    // A submittable element is a candidate for constraint validation
    // https://html.spec.whatwg.org/multipage/forms.html#category-submit
    // Submittable elements: button, input, select, textarea, form-associated custom elements [but not object]
    return false;
}

void HTMLObjectElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const&, Optional<FlyString> const&)
{
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element
    // Whenever one of the following conditions occur:
    if (
        // - the element's classid attribute is set, changed, or removed,
        (name == HTML::AttributeNames::classid) ||
        // - the element's classid attribute is not present, and its data attribute is set, changed, or removed,
        (!has_attribute(HTML::AttributeNames::classid) && name == HTML::AttributeNames::data) ||
        // - neither the element's classid attribute nor its data attribute are present, and its type attribute is set, changed, or removed,
        (!has_attribute(HTML::AttributeNames::classid) && !has_attribute(HTML::AttributeNames::data) && name == HTML::AttributeNames::type)) {
        // ...the user agent must queue an element task on the DOM manipulation task source given the object element to run
        // the following steps to (re)determine what the object element represents.
        queue_element_task_to_run_object_representation_steps();
    }
}

void HTMLObjectElement::form_associated_element_was_removed(DOM::Node*)
{
    destroy_the_child_navigable();
}

bool HTMLObjectElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::align,
        HTML::AttributeNames::border,
        HTML::AttributeNames::height,
        HTML::AttributeNames::hspace,
        HTML::AttributeNames::vspace,
        HTML::AttributeNames::width);
}

void HTMLObjectElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::align) {
            if (value.equals_ignoring_ascii_case("center"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Center));
            else if (value.equals_ignoring_ascii_case("middle"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Middle));
        } else if (name == HTML::AttributeNames::border) {
            if (auto parsed_value = parse_non_negative_integer(value); parsed_value.has_value()) {
                auto width_style_value = CSS::LengthStyleValue::create(CSS::Length::make_px(*parsed_value));
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopWidth, width_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightWidth, width_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomWidth, width_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftWidth, width_style_value);

                auto border_style_value = CSS::CSSKeywordValue::create(CSS::Keyword::Solid);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopStyle, border_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightStyle, border_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomStyle, border_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftStyle, border_style_value);
            }
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:maps-to-the-dimension-property-3
        else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, *parsed_value);
            }
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:maps-to-the-dimension-property
        else if (name == HTML::AttributeNames::hspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginLeft, *parsed_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginRight, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::vspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginTop, *parsed_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginBottom, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, *parsed_value);
            }
        }
    });
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#attr-object-data
String HTMLObjectElement::data() const
{
    auto data = get_attribute(HTML::AttributeNames::data);
    if (!data.has_value())
        return {};

    auto maybe_url = document().encoding_parse_url(*data);
    if (!maybe_url.has_value())
        return {};

    return maybe_url->to_string();
}

GC::Ptr<Layout::Node> HTMLObjectElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    switch (m_representation) {
    case Representation::Children:
        return NavigableContainer::create_layout_node(move(style));
    case Representation::ContentNavigable:
        return heap().allocate<Layout::NavigableContainerViewport>(document(), *this, move(style));
    case Representation::Image:
        if (image_data())
            return heap().allocate<Layout::ImageBox>(document(), *this, move(style), *this);
        break;
    default:
        break;
    }

    return nullptr;
}

void HTMLObjectElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

bool HTMLObjectElement::has_ancestor_media_element_or_object_element_not_showing_fallback_content() const
{
    for (auto const* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (is<HTMLMediaElement>(*ancestor))
            return true;

        if (is<HTMLObjectElement>(*ancestor)) {
            auto& ancestor_object = static_cast<HTMLObjectElement const&>(*ancestor);
            if (ancestor_object.m_representation != Representation::Children)
                return true;
        }
    }

    return false;
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:queue-an-element-task
void HTMLObjectElement::queue_element_task_to_run_object_representation_steps()
{
    // AD-HOC: If the document isn't fully active, this task will never run, and we will indefinitely delay the load event.
    if (!document().is_fully_active())
        return;

    // This task being queued or actively running must delay the load event of the element's node document.
    m_document_load_event_delayer_for_object_representation_task.emplace(document());

    queue_an_element_task(HTML::Task::Source::DOMManipulation, [this]() {
        ScopeGuard guard { [&]() { m_document_load_event_delayer_for_object_representation_task.clear(); } };

        auto& realm = this->realm();
        auto& vm = realm.vm();

        // FIXME: 1. If the user has indicated a preference that this object element's fallback content be shown instead of the
        //           element's usual behavior, then jump to the step below labeled fallback.

        // 2. If the element has an ancestor media element, or has an ancestor object element that is not showing its
        //    fallback content, or if the element is not in a document whose browsing context is non-null, or if the
        //    element's node document is not fully active, or if the element is still in the stack of open elements of
        //    an HTML parser or XML parser, or if the element is not being rendered, then jump to the step below labeled
        //    fallback.
        // FIXME: Handle the element being in the stack of open elements.
        // FIXME: Handle the element not being rendered.
        if (!document().browsing_context() || !document().is_fully_active()) {
            run_object_representation_fallback_steps();
            return;
        }
        if (has_ancestor_media_element_or_object_element_not_showing_fallback_content()) {
            run_object_representation_fallback_steps();
            return;
        }

        // 3. If the data attribute is present and its value is not the empty string, then:
        if (auto data = get_attribute(HTML::AttributeNames::data); data.has_value() && !data->is_empty()) {
            // 1. If the type attribute is present and its value is not a type that the user agent supports, then the user
            //    agent may jump to the step below labeled fallback without fetching the content to examine its real type.

            // 2. Let url be the result of encoding-parsing a URL given the data attribute's value, relative to the element's node document.
            auto url = document().encoding_parse_url(*data);

            // 3. If url is failure, then fire an event named error at the element and jump to the step below labeled fallback.
            if (!url.has_value()) {
                dispatch_event(DOM::Event::create(realm, HTML::EventNames::error));
                run_object_representation_fallback_steps();
                return;
            }

            // 4. Let request be a new request whose URL is url, client is the element's node document's relevant settings
            //    object, destination is "object", credentials mode is "include", mode is "navigate", initiator type is
            //    "object", and whose use-URL-credentials flag is set.
            auto request = Fetch::Infrastructure::Request::create(vm);
            request->set_url(url.release_value());
            request->set_client(&document().relevant_settings_object());
            request->set_destination(Fetch::Infrastructure::Request::Destination::Object);
            request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
            request->set_mode(Fetch::Infrastructure::Request::Mode::Navigate);
            request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Object);
            request->set_use_url_credentials(true);

            Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
            fetch_algorithms_input.process_response = [this](GC::Ref<Fetch::Infrastructure::Response> response) {
                auto& realm = this->realm();
                auto& global = document().realm().global_object();

                if (response->is_network_error()) {
                    resource_did_fail();
                    return;
                }

                if (response->type() == Fetch::Infrastructure::Response::Type::Opaque || response->type() == Fetch::Infrastructure::Response::Type::OpaqueRedirect) {
                    auto& filtered_response = static_cast<Fetch::Infrastructure::FilteredResponse&>(*response);
                    response = filtered_response.internal_response();
                }

                auto on_data_read = GC::create_function(realm.heap(), [this, response](ByteBuffer data) {
                    resource_did_load(response, data);
                });
                auto on_error = GC::create_function(realm.heap(), [this](JS::Value) {
                    resource_did_fail();
                });

                VERIFY(response->body());
                response->body()->fully_read(realm, on_data_read, on_error, GC::Ref { global });
            };

            // 5. Fetch request.
            auto result = Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));
            if (result.is_error()) {
                resource_did_fail();
                return;
            }

            //    Fetching the resource must delay the load event of the element's node document until the task that is
            //    queued by the networking task source once the resource has been fetched (defined next) has been run.
            m_document_load_event_delayer_for_resource_load.emplace(document());

            // 6. If the resource is not yet available (e.g. because the resource was not available in the cache, so that
            //    loading the resource required making a request over the network), then jump to the step below labeled
            //    fallback. The task that is queued by the networking task source once the resource is available must
            //    restart this algorithm from this step. Resources can load incrementally; user agents may opt to consider
            //    a resource "available" whenever enough data has been obtained to begin processing the resource.

            // NOTE: The request is always asynchronous, even if it is cached or succeeded/failed immediately. Allow the
            //       response callback to invoke the fallback steps. This prevents the fallback layout from flashing very
            //       briefly between here and the resource loading.
        }
    });
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:concept-event-fire-2
void HTMLObjectElement::resource_did_fail()
{
    ScopeGuard guard { [&]() { m_document_load_event_delayer_for_resource_load.clear(); } };

    // 3.7. If the load failed (e.g. there was an HTTP 404 error, there was a DNS error), fire an event named error at
    //      the element, then jump to the step below labeled fallback.
    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
    run_object_representation_fallback_steps();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#object-type-detection
void HTMLObjectElement::resource_did_load(Fetch::Infrastructure::Response const& response, ReadonlyBytes data)
{
    ScopeGuard guard { [&]() { m_document_load_event_delayer_for_resource_load.clear(); } };

    // 3.8. Determine the resource type, as follows:

    // 1. Let the resource type be unknown.
    Optional<MimeSniff::MimeType> resource_type;

    // FIXME: 3. If the user agent is configured to strictly obey Content-Type headers for this resource, and the resource has
    //           associated Content-Type metadata, then let the resource type be the type specified in the resource's Content-Type
    //           metadata, and jump to the step below labeled handler.

    // 3. Run the appropriate set of steps from the following list:
    // -> If the resource has associated Content-Type metadata
    if (auto content_type = response.header_list()->extract_mime_type(); content_type.has_value()) {
        // 1. Let binary be false.
        bool binary = false;

        // 2. If the type specified in the resource's Content-Type metadata is "text/plain", and the result of applying
        //    the rules for distinguishing if a resource is text or binary to the resource is that the resource is not
        //    text/plain, then set binary to true.
        if (content_type->essence() == "text/plain"sv) {
            auto computed_type = MimeSniff::Resource::sniff(
                data,
                MimeSniff::SniffingConfiguration {
                    .sniffing_context = MimeSniff::SniffingContext::TextOrBinary,
                    .supplied_type = content_type,
                });

            if (computed_type.essence() != "text/plain"sv)
                binary = true;
        }

        // 3. If the type specified in the resource's Content-Type metadata is "application/octet-stream", then set binary to true.
        else if (content_type->essence() == "application/octet-stream"sv) {
            binary = true;
        }

        // 4. If binary is false, then let the resource type be the type specified in the resource's Content-Type metadata,
        //    and jump to the step below labeled handler.
        if (!binary) {
            resource_type = move(content_type);
        }

        // 5. If there is a type attribute present on the object element, and its value is not application/octet-stream,
        //    then run the following steps:
        else if (auto type = this->type(); !type.is_empty() && (type != "application/octet-stream"sv)) {
            // 1. If the attribute's value is a type that starts with "image/" that is not also an XML MIME type, then
            //    let the resource type be the type specified in that type attribute.
            if (type.starts_with_bytes("image/"sv)) {
                auto parsed_type = MimeSniff::MimeType::parse(type);

                if (parsed_type.has_value() && !parsed_type->is_xml())
                    resource_type = move(parsed_type);
            }

            // 2. Jump to the step below labeled handler.
        }
    }
    // -> Otherwise, if the resource does not have associated Content-Type metadata
    else {
        Optional<MimeSniff::MimeType> tentative_type;

        // 1. If there is a type attribute present on the object element, then let the tentative type be the type specified in that type attribute.
        //    Otherwise, let tentative type be the computed type of the resource.
        if (auto type = this->type(); !type.is_empty())
            tentative_type = MimeSniff::MimeType::parse(type);
        else
            tentative_type = MimeSniff::Resource::sniff(data);

        // 2. If tentative type is not application/octet-stream, then let resource type be tentative type and jump to the
        //    step below labeled handler.
        if (tentative_type.has_value() && tentative_type->essence() != "application/octet-stream"sv)
            resource_type = move(tentative_type);
    }

    if (resource_type.has_value())
        run_object_representation_handler_steps(response, *resource_type, data);
    else
        run_object_representation_fallback_steps();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:plugin-11
void HTMLObjectElement::run_object_representation_handler_steps(Fetch::Infrastructure::Response const& response, MimeSniff::MimeType const& resource_type, ReadonlyBytes data)
{
    // 3.9. Handler: Handle the content as given by the first of the following cases that matches:

    // -> If the resource type is an XML MIME type, or if the resource type does not start with "image/"
    if (can_load_document_with_type(resource_type) && (resource_type.is_xml() || !resource_type.is_image())) {
        // If the object element's content navigable is null, then create a new child navigable for the element.
        if (!m_content_navigable && in_a_document_tree()) {
            MUST(create_new_child_navigable());
            set_content_navigable_has_session_history_entry_and_ready_for_navigation();
        }

        // NOTE: Creating a new nested browsing context can fail if the document is not attached to a browsing context
        if (!m_content_navigable)
            return;

        // Let response be the response from fetch.

        // If response's URL does not match about:blank, then navigate the element's content navigable to response's URL
        // using the element's node document, with historyHandling set to "replace".
        if (response.url().has_value() && !url_matches_about_blank(*response.url())) {
            MUST(m_content_navigable->navigate({
                .url = *response.url(),
                .source_document = document(),
                .history_handling = Bindings::NavigationHistoryBehavior::Replace,
            }));
        }

        // The object element represents its content navigable.
        run_object_representation_completed_steps(Representation::ContentNavigable);
    }

    // -> If the resource type starts with "image/", and support for images has not been disabled
    // FIXME: Handle disabling image support.
    else if (resource_type.is_image()) {
        // Destroy a child navigable given the object element.
        destroy_the_child_navigable();

        // Apply the image sniffing rules to determine the type of the image.
        // The object element represents the specified image.
        // If the image cannot be rendered, e.g. because it is malformed or in an unsupported format, jump to the step
        // below labeled fallback.
        if (data.is_empty()) {
            run_object_representation_fallback_steps();
            return;
        }

        load_image();
    }

    // -> Otherwise
    else {
        // The given resource type is not supported. Jump to the step below labeled fallback.
        run_object_representation_fallback_steps();
    }
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:the-object-element-19
void HTMLObjectElement::run_object_representation_completed_steps(Representation representation)
{
    // 3.10. The element's contents are not part of what the object element represents.

    // 3.11. If the object element does not represent its content navigable, then once the resource is completely loaded,
    //       queue an element task on the DOM manipulation task source given the object element to fire an event named
    //       load at the element.
    if (representation != Representation::ContentNavigable) {
        queue_an_element_task(HTML::Task::Source::DOMManipulation, [&]() {
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));
        });
    }

    update_layout_and_child_objects(representation);

    // 3.12. Return.
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:the-object-element-23
void HTMLObjectElement::run_object_representation_fallback_steps()
{
    // 4. Fallback: The object element represents the element's children. This is the element's fallback content.
    //    Destroy a child navigable given the element.
    destroy_the_child_navigable();

    update_layout_and_child_objects(Representation::Children);
}

void HTMLObjectElement::load_image()
{
    // FIXME: This currently reloads the image instead of reusing the resource we've already downloaded.
    auto data = get_attribute_value(HTML::AttributeNames::data);
    auto url = document().encoding_parse_url(data);

    if (!url.has_value()) {
        run_object_representation_fallback_steps();
        return;
    }

    m_resource_request = HTML::SharedResourceRequest::get_or_create(realm(), document().page(), *url);
    m_resource_request->add_callbacks(
        [this] {
            run_object_representation_completed_steps(Representation::Image);
        },
        [this] {
            run_object_representation_fallback_steps();
        });

    if (m_resource_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(vm(), *url, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document().relevant_settings_object());
        m_resource_request->fetch_resource(realm(), request);
    }
}

void HTMLObjectElement::update_layout_and_child_objects(Representation representation)
{
    if (representation == Representation::Children) {
        for_each_child_of_type<HTMLObjectElement>([](auto& object) {
            object.queue_element_task_to_run_object_representation_steps();
            return IterationDecision::Continue;
        });
    }

    m_representation = representation;
    invalidate_style(DOM::StyleInvalidationReason::HTMLObjectElementUpdateLayoutAndChildObjects);
    set_needs_layout_tree_update(true);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLObjectElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

GC::Ptr<DecodedImageData> HTMLObjectElement::image_data() const
{
    if (!m_resource_request)
        return nullptr;
    return m_resource_request->image_data();
}

bool HTMLObjectElement::is_image_available() const
{
    return image_data() != nullptr;
}

Optional<CSSPixels> HTMLObjectElement::intrinsic_width() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_width();
    return {};
}

Optional<CSSPixels> HTMLObjectElement::intrinsic_height() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_height();
    return {};
}

Optional<CSSPixelFraction> HTMLObjectElement::intrinsic_aspect_ratio() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_aspect_ratio();
    return {};
}

RefPtr<Gfx::ImmutableBitmap> HTMLObjectElement::current_image_bitmap(Gfx::IntSize size) const
{
    if (auto image_data = this->image_data())
        return image_data->bitmap(0, size);
    return nullptr;
}

void HTMLObjectElement::set_visible_in_viewport(bool)
{
    // FIXME: Loosen grip on image data when it's not visible, e.g via volatile memory.
}

}
