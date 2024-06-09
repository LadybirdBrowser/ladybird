/*
 * Copyright (c) 2020-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/HTMLObjectElementPrototype.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoading.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/MimeSniff/Resource.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLObjectElement);

HTMLObjectElement::HTMLObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : NavigableContainer(document, move(qualified_name))
{
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element
    // Whenever one of the following conditions occur:
    // - the element is created,
    // ...the user agent must queue an element task on the DOM manipulation task source given
    // the object element to run the following steps to (re)determine what the object element represents.
    // This task being queued or actively running must delay the load event of the element's node document.
    queue_element_task_to_run_object_representation_steps();
}

HTMLObjectElement::~HTMLObjectElement() = default;

void HTMLObjectElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLObjectElement);
}

void HTMLObjectElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_image_request);
}

void HTMLObjectElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const&)
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

        // ...the user agent must queue an element task on the DOM manipulation task source given
        // the object element to run the following steps to (re)determine what the object element represents.
        // This task being queued or actively running must delay the load event of the element's node document.
        queue_element_task_to_run_object_representation_steps();
    }
}

void HTMLObjectElement::form_associated_element_was_removed(DOM::Node*)
{
    destroy_the_child_navigable();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#attr-object-data
String HTMLObjectElement::data() const
{
    auto data = get_attribute_value(HTML::AttributeNames::data);
    return MUST(document().parse_url(data).to_string());
}

JS::GCPtr<Layout::Node> HTMLObjectElement::create_layout_node(NonnullRefPtr<CSS::StyleProperties> style)
{
    switch (m_representation) {
    case Representation::Children:
        return NavigableContainer::create_layout_node(move(style));
    case Representation::NestedBrowsingContext:
        // FIXME: Actually paint the nested browsing context's document, similar to how iframes are painted with FrameBox and NestedBrowsingContextPaintable.
        return nullptr;
    case Representation::Image:
        if (image_data())
            return heap().allocate_without_realm<Layout::ImageBox>(document(), *this, move(style), *this);
        break;
    default:
        break;
    }

    return nullptr;
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
    queue_an_element_task(HTML::Task::Source::DOMManipulation, [&]() {
        // FIXME: 1. If the user has indicated a preference that this object element's fallback content be shown instead of the element's usual behavior, then jump to the step below labeled fallback.

        // 2. If the element has an ancestor media element, or has an ancestor object element that is not showing its fallback content, or if the element is not in a document whose browsing context is non-null, or if the element's node document is not fully active, or if the element is still in the stack of open elements of an HTML parser or XML parser, or if the element is not being rendered, then jump to the step below labeled fallback.
        if (!document().browsing_context() || !document().is_fully_active())
            return run_object_representation_fallback_steps();
        if (has_ancestor_media_element_or_object_element_not_showing_fallback_content())
            return run_object_representation_fallback_steps();

        // FIXME: 3. If the classid attribute is present, and has a value that isn't the empty string, then: if the user agent can find a plugin suitable according to the value of the classid attribute, and plugins aren't being sandboxed, then that plugin should be used, and the value of the data attribute, if any, should be passed to the plugin. If no suitable plugin can be found, or if the plugin reports an error, jump to the step below labeled fallback.

        // 4. If the data attribute is present and its value is not the empty string, then:
        if (auto maybe_data = get_attribute(HTML::AttributeNames::data); maybe_data.has_value() && !maybe_data->is_empty()) {
            // 1. If the type attribute is present and its value is not a type that the user agent supports, and is not a type that the user agent can find a plugin for, then the user agent may jump to the step below labeled fallback without fetching the content to examine its real type.

            // 2. Parse a URL given the data attribute, relative to the element's node document.
            auto url = document().parse_url(*maybe_data);

            // 3. If that failed, fire an event named error at the element, then jump to the step below labeled fallback.
            if (!url.is_valid()) {
                dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
                return run_object_representation_fallback_steps();
            }

            // 4. Let request be a new request whose URL is the resulting URL record, client is the element's node document's relevant settings object, destination is "object", credentials mode is "include", mode is "navigate", and whose use-URL-credentials flag is set.
            auto request = LoadRequest::create_for_url_on_page(url, &document().page());

            // 5. Fetch request, with processResponseEndOfBody given response res set to finalize and report timing with res, the element's node document's relevant global object, and "object".
            //    Fetching the resource must delay the load event of the element's node document until the task that is queued by the networking task source once the resource has been fetched (defined next) has been run.
            set_resource(ResourceLoader::the().load_resource(Resource::Type::Generic, request));

            // 6. If the resource is not yet available (e.g. because the resource was not available in the cache, so that loading the resource required making a request over the network), then jump to the step below labeled fallback. The task that is queued by the networking task source once the resource is available must restart this algorithm from this step. Resources can load incrementally; user agents may opt to consider a resource "available" whenever enough data has been obtained to begin processing the resource.

            // NOTE: The request is always asynchronous, even if it is cached or succeeded/failed immediately. Allow the callbacks below to invoke
            //       the fallback steps. This prevents the fallback layout from flashing very briefly between here and the resource loading.
            return;
        }

        // 5. If the data attribute is absent but the type attribute is present, and the user agent can find a plugin suitable according to the value of the type attribute, and plugins aren't being sandboxed, then that plugin should be used. If these conditions cannot be met, or if the plugin reports an error, jump to the step below labeled fallback. Otherwise return; once the plugin is completely loaded, queue an element task on the DOM manipulation task source given the object element to fire an event named load at the element.
        run_object_representation_fallback_steps();
    });
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:concept-event-fire-2
void HTMLObjectElement::resource_did_fail()
{
    // 4.7. If the load failed (e.g. there was an HTTP 404 error, there was a DNS error), fire an event named error at the element, then jump to the step below labeled fallback.
    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
    run_object_representation_fallback_steps();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#object-type-detection
void HTMLObjectElement::resource_did_load()
{
    // 4.8. Determine the resource type, as follows:

    // 1. Let the resource type be unknown.
    Optional<String> resource_type;

    // FIXME: 2. If the user agent is configured to strictly obey Content-Type headers for this resource, and the resource has associated Content-Type metadata, then let the resource type be the type specified in the resource's Content-Type metadata, and jump to the step below labeled handler.
    // FIXME: 3. If there is a type attribute present on the object element, and that attribute's value is not a type that the user agent supports, but it is a type that a plugin supports, then let the resource type be the type specified in that type attribute, and jump to the step below labeled handler.

    // 4. Run the appropriate set of steps from the following list:
    // * If the resource has associated Content-Type metadata
    if (auto maybe_content_type = resource()->response_headers().get("Content-Type"sv); maybe_content_type.has_value()) {
        auto& content_type = maybe_content_type.value();

        // 1. Let binary be false.
        bool binary = false;

        // 2. If the type specified in the resource's Content-Type metadata is "text/plain", and the result of applying the rules for distinguishing if a resource is text or binary to the resource is that the resource is not text/plain, then set binary to true.
        if (content_type == "text/plain"sv) {
            auto supplied_type = MimeSniff::MimeType::parse(content_type).release_value_but_fixme_should_propagate_errors();
            auto computed_type = MimeSniff::Resource::sniff(resource()->encoded_data(), MimeSniff::SniffingConfiguration {
                                                                                            .sniffing_context = MimeSniff::SniffingContext::TextOrBinary,
                                                                                            .supplied_type = move(supplied_type),
                                                                                        })
                                     .release_value_but_fixme_should_propagate_errors();
            if (computed_type.essence() != "text/plain"sv)
                binary = true;
        }

        // 3. If the type specified in the resource's Content-Type metadata is "application/octet-stream", then set binary to true.
        if (content_type == "application/octet-stream"sv)
            binary = true;

        // 4. If binary is false, then let the resource type be the type specified in the resource's Content-Type metadata, and jump to the step below labeled handler.
        if (!binary)
            return run_object_representation_handler_steps(content_type);

        // 5. If there is a type attribute present on the object element, and its value is not application/octet-stream, then run the following steps:
        if (auto type = this->type(); !type.is_empty() && (type != "application/octet-stream"sv)) {
            // 1. If the attribute's value is a type that a plugin supports, or the attribute's value is a type that starts with "image/" that is not also an XML MIME type, then let the resource type be the type specified in that type attribute.
            // FIXME: This only partially implements this step.
            if (type.starts_with_bytes("image/"sv))
                resource_type = move(type);

            // 2. Jump to the step below labeled handler.
        }
    }
    // * Otherwise, if the resource does not have associated Content-Type metadata
    else {
        Optional<String> tentative_type;

        // 1. If there is a type attribute present on the object element, then let the tentative type be the type specified in that type attribute.
        //    Otherwise, let tentative type be the computed type of the resource.
        if (auto type = this->type(); !type.is_empty())
            tentative_type = move(type);

        // FIXME: For now, ignore application/ MIME types as we cannot render yet them anyways. We will need to implement the MIME type sniffing
        //        algorithm in order to map all unknown MIME types to "application/octet-stream".
        else if (auto type = resource()->mime_type(); !type.starts_with("application/"sv))
            tentative_type = MUST(String::from_byte_string(type));

        // 2. If tentative type is not application/octet-stream, then let resource type be tentative type and jump to the step below labeled handler.
        if (tentative_type.has_value() && tentative_type != "application/octet-stream"sv)
            resource_type = move(tentative_type);
    }

    // FIXME: 5. If applying the URL parser algorithm to the URL of the specified resource (after any redirects) results in a URL record whose path component matches a pattern that a plugin supports, then let resource type be the type that that plugin can handle.
    run_object_representation_handler_steps(resource_type.has_value() ? resource_type->to_byte_string() : ByteString::empty());
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:plugin-11
void HTMLObjectElement::run_object_representation_handler_steps(Optional<ByteString> resource_type)
{
    // 4.9. Handler: Handle the content as given by the first of the following cases that matches:

    // * FIXME: If the resource type is not a type that the user agent supports, but it is a type that a plugin supports
    //     If the object element's nested browsing context is non-null, then it must be discarded and then set to null.
    //     If plugins are being sandboxed, then jump to the step below labeled fallback.
    //     Otherwise, the user agent should use the plugin that supports resource type and pass the content of the resource to that plugin. If the plugin reports an error, then jump to the step below labeled fallback.

    if (!resource_type.has_value()) {
        run_object_representation_fallback_steps();
        return;
    }
    auto mime_type = MimeSniff::MimeType::parse(*resource_type).release_value_but_fixme_should_propagate_errors();

    // * If the resource type is an XML MIME type, or if the resource type does not start with "image/"
    if (mime_type.has_value() && can_load_document_with_type(*mime_type) && (mime_type->is_xml() || !mime_type->is_image())) {
        // If the object element's content navigable is null, then create a new child navigable for the element.
        if (!m_content_navigable && in_a_document_tree()) {
            MUST(create_new_child_navigable());
            set_content_navigable_initialized();
        }

        // NOTE: Creating a new nested browsing context can fail if the document is not attached to a browsing context
        if (!m_content_navigable)
            return;

        // If the URL of the given resource does not match about:blank, then navigate the element's nested browsing context to that resource, with historyHandling set to "replace" and the source browsing context set to the object element's node document's browsing context. (The data attribute of the object element doesn't get updated if the browsing context gets further navigated to other locations.)
        if (auto const& url = resource()->url(); url != "about:blank"sv)
            MUST(m_content_navigable->navigate({ .url = url,
                .source_document = document(),
                .history_handling = Bindings::NavigationHistoryBehavior::Replace }));

        // The object element represents its nested browsing context.
        run_object_representation_completed_steps(Representation::NestedBrowsingContext);
    }

    // * If the resource type starts with "image/", and support for images has not been disabled
    // FIXME: Handle disabling image support.
    else if (resource_type.has_value() && resource_type->starts_with("image/"sv)) {
        // Destroy the child navigable of the object element.
        destroy_the_child_navigable();

        // Apply the image sniffing rules to determine the type of the image.
        // The object element represents the specified image.
        // If the image cannot be rendered, e.g. because it is malformed or in an unsupported format, jump to the step below labeled fallback.
        if (!resource()->has_encoded_data())
            return run_object_representation_fallback_steps();

        load_image();
    }

    // * Otherwise
    else {
        // The given resource type is not supported. Jump to the step below labeled fallback.
        run_object_representation_fallback_steps();
    }
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:the-object-element-19
void HTMLObjectElement::run_object_representation_completed_steps(Representation representation)
{
    // 4.10. The element's contents are not part of what the object element represents.
    // 4.11. If the object element does not represent its nested browsing context, then once the resource is completely loaded, queue an element task on the DOM manipulation task source given the object element to fire an event named load at the element.
    if (representation != Representation::NestedBrowsingContext) {
        queue_an_element_task(HTML::Task::Source::DOMManipulation, [&]() {
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));
        });
    }

    update_layout_and_child_objects(representation);

    // 4.12. Return.
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-object-element:the-object-element-23
void HTMLObjectElement::run_object_representation_fallback_steps()
{
    // 4. Fallback: The object element represents the element's children. This is the element's fallback content. Destroy the child navigable for the element.
    destroy_the_child_navigable();

    update_layout_and_child_objects(Representation::Children);
}

void HTMLObjectElement::load_image()
{
    // NOTE: This currently reloads the image instead of reusing the resource we've already downloaded.
    auto data = get_attribute_value(HTML::AttributeNames::data);
    auto url = document().parse_url(data);
    m_image_request = HTML::SharedImageRequest::get_or_create(realm(), document().page(), url);
    m_image_request->add_callbacks(
        [this] {
            run_object_representation_completed_steps(Representation::Image);
        },
        [this] {
            run_object_representation_fallback_steps();
        });

    if (m_image_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(vm(), url, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document().relevant_settings_object());
        m_image_request->fetch_image(realm(), request);
    }
}

void HTMLObjectElement::update_layout_and_child_objects(Representation representation)
{
    if ((m_representation == Representation::Children && representation != Representation::Children)
        || (m_representation != Representation::Children && representation == Representation::Children)) {
        for_each_child_of_type<HTMLObjectElement>([](auto& object) {
            object.queue_element_task_to_run_object_representation_steps();
            return IterationDecision::Continue;
        });
    }

    m_representation = representation;
    invalidate_style();
    document().invalidate_layout();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLObjectElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

JS::GCPtr<DecodedImageData> HTMLObjectElement::image_data() const
{
    if (!m_image_request)
        return nullptr;
    return m_image_request->image_data();
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
