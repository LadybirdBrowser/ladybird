/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/HTMLLinkElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLLinkElement);

HTMLLinkElement::HTMLLinkElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLLinkElement::~HTMLLinkElement() = default;

void HTMLLinkElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLLinkElement);
}

void HTMLLinkElement::removed_from(Node* old_parent)
{
    Base::removed_from(old_parent);
    if (m_loaded_style_sheet) {
        document_or_shadow_root_style_sheets().remove_a_css_style_sheet(*m_loaded_style_sheet);
        m_loaded_style_sheet = nullptr;
    }
}

void HTMLLinkElement::inserted()
{
    HTMLElement::inserted();

    if (!document().browsing_context()) {
        return;
    }

    if (m_relationship & Relationship::Stylesheet) {
        // https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:fetch-and-process-the-linked-resource
        // The appropriate times to fetch and process this type of link are:
        //  - When the external resource link is created on a link element that is already browsing-context connected.
        //  - When the external resource link's link element becomes browsing-context connected.
        fetch_and_process_linked_resource();
    }

    // FIXME: Follow spec for fetching and processing these attributes as well
    if (m_relationship & Relationship::Preload) {
        // FIXME: Respect the "as" attribute.
        LoadRequest request;
        request.set_url(document().parse_url(get_attribute_value(HTML::AttributeNames::href)));
        set_resource(ResourceLoader::the().load_resource(Resource::Type::Generic, request));
    } else if (m_relationship & Relationship::DNSPrefetch) {
        ResourceLoader::the().prefetch_dns(document().parse_url(get_attribute_value(HTML::AttributeNames::href)));
    } else if (m_relationship & Relationship::Preconnect) {
        ResourceLoader::the().preconnect(document().parse_url(get_attribute_value(HTML::AttributeNames::href)));
    } else if (m_relationship & Relationship::Icon) {
        auto favicon_url = document().parse_url(href());
        auto favicon_request = LoadRequest::create_for_url_on_page(favicon_url, &document().page());
        set_resource(ResourceLoader::the().load_resource(Resource::Type::Generic, favicon_request));
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-link-as
String HTMLLinkElement::as() const
{
    String attribute_value = get_attribute_value(HTML::AttributeNames::as);

    if (attribute_value.equals_ignoring_ascii_case("fetch"sv)
        || attribute_value.equals_ignoring_ascii_case("image"sv)
        || attribute_value.equals_ignoring_ascii_case("script"sv)
        || attribute_value.equals_ignoring_ascii_case("style"sv)
        || attribute_value.equals_ignoring_ascii_case("video"sv)
        || attribute_value.equals_ignoring_ascii_case("audio"sv)
        || attribute_value.equals_ignoring_ascii_case("track"sv)
        || attribute_value.equals_ignoring_ascii_case("font"sv)) {
        return attribute_value.to_lowercase().release_value();
    }

    return String {};
}

WebIDL::ExceptionOr<void> HTMLLinkElement::set_as(String const& value)
{
    return set_attribute(HTML::AttributeNames::as, move(value));
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-link-rellist
GC::Ref<DOM::DOMTokenList> HTMLLinkElement::rel_list()
{
    // The relList IDL attribute must reflect the rel content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-link-sizes
GC::Ref<DOM::DOMTokenList> HTMLLinkElement::sizes()
{
    // The size IDL attribute must reflect the size content attribute.
    if (!m_sizes)
        m_sizes = DOM::DOMTokenList::create(*this, HTML::AttributeNames::sizes);
    return *m_sizes;
}

bool HTMLLinkElement::has_loaded_icon() const
{
    return m_relationship & Relationship::Icon && resource() && resource()->is_loaded() && resource()->has_encoded_data();
}

void HTMLLinkElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // 4.6.7 Link types - https://html.spec.whatwg.org/multipage/links.html#linkTypes
    if (name == HTML::AttributeNames::rel) {
        m_relationship = 0;
        // Keywords are always ASCII case-insensitive, and must be compared as such.
        auto lowercased_value = value.value_or(String {}).to_ascii_lowercase();
        // To determine which link types apply to a link, a, area, or form element,
        // the element's rel attribute must be split on ASCII whitespace.
        // The resulting tokens are the keywords for the link types that apply to that element.
        auto parts = lowercased_value.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
        for (auto& part : parts) {
            if (part == "stylesheet"sv)
                m_relationship |= Relationship::Stylesheet;
            else if (part == "alternate"sv)
                m_relationship |= Relationship::Alternate;
            else if (part == "preload"sv)
                m_relationship |= Relationship::Preload;
            else if (part == "dns-prefetch"sv)
                m_relationship |= Relationship::DNSPrefetch;
            else if (part == "preconnect"sv)
                m_relationship |= Relationship::Preconnect;
            else if (part == "icon"sv)
                m_relationship |= Relationship::Icon;
        }

        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.value_or(String {}));
    }

    // https://html.spec.whatwg.org/multipage/semantics.html#the-link-element:explicitly-enabled
    // Whenever the disabled attribute is removed, set the link element's explicitly enabled attribute to true.
    if (!value.has_value() && name == HTML::AttributeNames::disabled)
        m_explicitly_enabled = true;

    if (m_relationship & Relationship::Stylesheet) {
        if (name == HTML::AttributeNames::disabled && m_loaded_style_sheet)
            document_or_shadow_root_style_sheets().remove_a_css_style_sheet(*m_loaded_style_sheet);

        // https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:fetch-and-process-the-linked-resource
        // The appropriate times to fetch and process this type of link are:
        if (
            is_browsing_context_connected()
            && (
                // AD-HOC: When the rel attribute changes
                name == AttributeNames::rel ||
                // - When the href attribute of the link element of an external resource link that is already browsing-context connected is changed.
                name == AttributeNames::href ||
                // - When the disabled attribute of the link element of an external resource link that is already browsing-context connected is set, changed, or removed.
                name == AttributeNames::disabled ||
                // - When the crossorigin attribute of the link element of an external resource link that is already browsing-context connected is set, changed, or removed.
                name == AttributeNames::crossorigin
                // FIXME: - When the type attribute of the link element of an external resource link that is already browsing-context connected is set or changed to a value that does not or no longer matches the Content-Type metadata of the previous obtained external resource, if any.
                // FIXME: - When the type attribute of the link element of an external resource link that is already browsing-context connected, but was previously not obtained due to the type attribute specifying an unsupported type, is removed or changed.
                )) {
            fetch_and_process_linked_resource();
        }

        if (name == HTML::AttributeNames::media && m_loaded_style_sheet) {
            m_loaded_style_sheet->set_media(value.value_or(String {}));
        }
    }
}

void HTMLLinkElement::resource_did_fail()
{
    dbgln_if(CSS_LOADER_DEBUG, "HTMLLinkElement: Resource did fail. URL: {}", resource()->url());
    if (m_relationship & Relationship::Preload) {
        dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::error));
    }
}

void HTMLLinkElement::resource_did_load()
{
    VERIFY(resource());
    if (m_relationship & Relationship::Icon) {
        resource_did_load_favicon();
        m_document_load_event_delayer.clear();
    }
    if (m_relationship & Relationship::Preload) {
        dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::load));
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#create-link-options-from-element
HTMLLinkElement::LinkProcessingOptions HTMLLinkElement::create_link_options()
{
    // 1. Let document be el's node document.
    auto& document = this->document();

    // 2. Let options be a new link processing options with
    LinkProcessingOptions options;
    // FIXME: destination                      the result of translating the state of el's as attribute
    // crossorigin                      the state of el's crossorigin content attribute
    options.crossorigin = cors_setting_attribute_from_keyword(get_attribute(AttributeNames::crossorigin));
    // referrer policy                  the state of el's referrerpolicy content attribute
    options.referrer_policy = ReferrerPolicy::from_string(get_attribute(AttributeNames::referrerpolicy).value_or(""_string)).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);
    // FIXME: source set                       el's source set
    // base URL                         document's document base URL
    options.base_url = document.base_url();
    // origin                           document's origin
    options.origin = document.origin();
    // environment                      document's relevant settings object
    options.environment = &document.relevant_settings_object();
    // policy container                 document's policy container
    options.policy_container = document.policy_container();
    // document                         document
    options.document = &document;
    // FIXME: cryptographic nonce metadata     The current value of el's [[CryptographicNonce]] internal slot
    // fetch priority                   the state of el's fetchpriority content attribute
    options.fetch_priority = Fetch::Infrastructure::request_priority_from_string(get_attribute_value(HTML::AttributeNames::fetchpriority)).value_or(Fetch::Infrastructure::Request::Priority::Auto);

    // 3. If el has an href attribute, then set options's href to the value of el's href attribute.
    if (auto maybe_href = get_attribute(AttributeNames::href); maybe_href.has_value())
        options.href = maybe_href.value();

    // 4. If el has an integrity attribute, then set options's integrity to the value of el's integrity content attribute.
    if (auto maybe_integrity = get_attribute(AttributeNames::integrity); maybe_integrity.has_value())
        options.integrity = maybe_integrity.value();

    // 5. If el has a type attribute, then set options's type to the value of el's type attribute.
    if (auto maybe_type = get_attribute(AttributeNames::type); maybe_type.has_value())
        options.type = maybe_type.value();

    // FIXME: 6. Assert: options's href is not the empty string, or options's source set is not null.
    //           A link element with neither an href or an imagesrcset does not represent a link.

    // 7. Return options.
    return options;
}

// https://html.spec.whatwg.org/multipage/semantics.html#create-a-link-request
GC::Ptr<Fetch::Infrastructure::Request> HTMLLinkElement::create_link_request(HTMLLinkElement::LinkProcessingOptions const& options)
{
    // 1. Assert: options's href is not the empty string.
    VERIFY(!options.href.is_empty());

    // FIXME: 2. If options's destination is null, then return null.

    // 3. Let url be the result of encoding-parsing a URL given options's href, relative to options's base URL.
    auto url = options.base_url.complete_url(options.href);

    // 4. If url is failure, then return null.
    if (!url.is_valid())
        return nullptr;

    // 5. Let request be the result of creating a potential-CORS request given url, options's destination, and options's crossorigin.
    auto request = create_potential_CORS_request(vm(), url, options.destination, options.crossorigin);

    // 6. Set request's policy container to options's policy container.
    request->set_policy_container(options.policy_container);

    // 7. Set request's integrity metadata to options's integrity.
    request->set_integrity_metadata(options.integrity);

    // 8. Set request's cryptographic nonce metadata to options's cryptographic nonce metadata.
    request->set_cryptographic_nonce_metadata(options.cryptographic_nonce_metadata);

    // 9. Set request's referrer policy to options's referrer policy.
    request->set_referrer_policy(options.referrer_policy);

    // 10. Set request's client to options's environment.
    request->set_client(options.environment);

    // 11. Set request's priority to options's fetch priority.
    request->set_priority(options.fetch_priority);

    // 12. Return request.
    return request;
}

// https://html.spec.whatwg.org/multipage/semantics.html#fetch-and-process-the-linked-resource
void HTMLLinkElement::fetch_and_process_linked_resource()
{
    default_fetch_and_process_linked_resource();
}

// https://html.spec.whatwg.org/multipage/semantics.html#default-fetch-and-process-the-linked-resource
void HTMLLinkElement::default_fetch_and_process_linked_resource()
{
    // https://html.spec.whatwg.org/multipage/semantics.html#the-link-element:attr-link-href-4
    // If both the href and imagesrcset attributes are absent, then the element does not define a link.
    // FIXME: Support imagesrcset attribute
    if (!has_attribute(AttributeNames::href) || href().is_empty())
        return;

    // 1. Let options be the result of creating link options from el.
    auto options = create_link_options();

    // 2. Let request be the result of creating a link request given options.
    auto request = create_link_request(options);

    // 3. If request is null, then return.
    if (request == nullptr) {
        return;
    }

    // FIXME: 4. Set request's synchronous flag.

    // 5. Run the linked resource fetch setup steps, given el and request. If the result is false, then return.
    if (!linked_resource_fetch_setup_steps(*request))
        return;

    // 6. Set request's initiator type to "css" if el's rel attribute contains the keyword stylesheet; "link" otherwise.
    if (m_relationship & Relationship::Stylesheet) {
        request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::CSS);
    } else {
        request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Link);
    }

    // 7. Fetch request with processResponseConsumeBody set to the following steps given response response and null, failure, or a byte sequence bodyBytes:
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_consume_body = [this, hr = options](auto response, auto body_bytes) {
        // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
        //        https://github.com/whatwg/html/issues/9355
        response = response->unsafe_response();

        // 1. Let success be true.
        bool success = true;

        // 2. If either of the following conditions are met:
        // - bodyBytes is null or failure; or
        // - response's status is not an ok status,
        if (body_bytes.template has<Empty>() || body_bytes.template has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>() || !Fetch::Infrastructure::is_ok_status(response->status())) {
            // then set success to false.
            success = false;
        }

        // FIXME: 3. Otherwise, wait for the link resource's critical subresources to finish loading.

        // 4. Process the linked resource given el, success, response, and bodyBytes.
        process_linked_resource(success, response, body_bytes);
    };

    if (m_fetch_controller)
        m_fetch_controller->abort(realm(), {});
    m_fetch_controller = MUST(Fetch::Fetching::fetch(realm(), *request, Fetch::Infrastructure::FetchAlgorithms::create(vm(), move(fetch_algorithms_input))));
}

// https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:process-the-linked-resource
void HTMLLinkElement::process_stylesheet_resource(bool success, Fetch::Infrastructure::Response const& response, Variant<Empty, Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag, ByteBuffer> body_bytes)
{
    // 1. If the resource's Content-Type metadata is not text/css, then set success to false.
    auto extracted_mime_type = response.header_list()->extract_mime_type();
    if (!extracted_mime_type.has_value() || extracted_mime_type->essence() != "text/css") {
        success = false;
    }

    // FIXME: 2. If el no longer creates an external resource link that contributes to the styling processing model,
    //           or if, since the resource in question was fetched, it has become appropriate to fetch it again, then return.

    // 3. If el has an associated CSS style sheet, remove the CSS style sheet.
    if (m_loaded_style_sheet) {
        document_or_shadow_root_style_sheets().remove_a_css_style_sheet(*m_loaded_style_sheet);
        m_loaded_style_sheet = nullptr;
    }

    // 4. If success is true, then:
    if (success) {
        // 1. Create a CSS style sheet with the following properties:
        //        type
        //            text/css
        //        location
        //            response's URL list[0]
        //        owner node
        //            element
        //        media
        //            The media attribute of element.
        //        title
        //            The title attribute of element, if element is in a document tree, or the empty string otherwise.
        //        alternate flag
        //            Set if the link is an alternative style sheet and element's explicitly enabled is false; unset otherwise.
        //        origin-clean flag
        //            Set if the resource is CORS-same-origin; unset otherwise.
        //        parent CSS style sheet
        //        owner CSS rule
        //            null
        //        disabled flag
        //            Left at its default value.
        //        CSS rules
        //          Left uninitialized.
        //
        // The CSS environment encoding is the result of running the following steps: [CSSSYNTAX]
        //     1. If the element has a charset attribute, get an encoding from that attribute's value. If that succeeds, return the resulting encoding. [ENCODING]
        //     2. Otherwise, return the document's character encoding. [DOM]

        Optional<String> encoding;
        if (auto charset = attribute(HTML::AttributeNames::charset); charset.has_value())
            encoding = charset.release_value();

        if (!encoding.has_value())
            encoding = document().encoding_or_default();

        auto decoder = TextCodec::decoder_for(*encoding);

        if (!decoder.has_value()) {
            // If we don't support the encoding yet, let's error out instead of trying to decode it as something it's most likely not.
            dbgln("FIXME: Style sheet encoding '{}' is not supported yet", encoding);
            dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::error));
        } else {
            auto const& encoded_string = body_bytes.get<ByteBuffer>();
            auto maybe_decoded_string = TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, encoded_string);
            if (maybe_decoded_string.is_error()) {
                dbgln("Style sheet {} claimed to be '{}' but decoding failed", response.url().value_or(URL::URL()), encoding);
                dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::error));
            } else {
                auto const decoded_string = maybe_decoded_string.release_value();
                m_loaded_style_sheet = parse_css_stylesheet(CSS::Parser::ParsingContext(document(), *response.url()), decoded_string);

                if (m_loaded_style_sheet) {
                    Optional<String> location;
                    if (!response.url_list().is_empty())
                        location = MUST(response.url_list().first().to_string());

                    document().style_sheets().create_a_css_style_sheet(
                        "text/css"_string,
                        this,
                        attribute(HTML::AttributeNames::media).value_or({}),
                        in_a_document_tree() ? attribute(HTML::AttributeNames::title).value_or({}) : String {},
                        m_relationship & Relationship::Alternate && !m_explicitly_enabled,
                        true,
                        move(location),
                        nullptr,
                        nullptr,
                        *m_loaded_style_sheet);
                } else {
                    dbgln_if(CSS_LOADER_DEBUG, "HTMLLinkElement: Failed to parse stylesheet: {}", resource()->url());
                }

                // 2. Fire an event named load at el.
                dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::load));
            }
        }
    }
    // 5. Otherwise, fire an event named error at el.
    else {
        dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::error));
    }

    // FIXME: 6. If el contributes a script-blocking style sheet, then:
    //     FIXME: 1. Assert: el's node document's script-blocking style sheet counter is greater than 0.
    //     FIXME: 2. Decrement el's node document's script-blocking style sheet counter by 1.

    // 7. Unblock rendering on el.
    m_document_load_event_delayer.clear();
}

// https://html.spec.whatwg.org/multipage/semantics.html#process-the-linked-resource
void HTMLLinkElement::process_linked_resource(bool success, Fetch::Infrastructure::Response const& response, Variant<Empty, Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag, ByteBuffer> body_bytes)
{
    if (m_relationship & Relationship::Stylesheet)
        process_stylesheet_resource(success, response, body_bytes);
}

// https://html.spec.whatwg.org/multipage/semantics.html#linked-resource-fetch-setup-steps
bool HTMLLinkElement::linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request& request)
{
    if (m_relationship & Relationship::Stylesheet)
        return stylesheet_linked_resource_fetch_setup_steps(request);

    return true;
}

// https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:linked-resource-fetch-setup-steps
bool HTMLLinkElement::stylesheet_linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request& request)
{
    // 1. If el's disabled attribute is set, then return false.
    if (has_attribute(AttributeNames::disabled))
        return false;
    // FIXME: 2. If el contributes a script-blocking style sheet, increment el's node document's script-blocking style sheet counter by 1.

    // 3. If el's media attribute's value matches the environment and el is potentially render-blocking, then block rendering on el.
    // FIXME: Check media attribute value.
    m_document_load_event_delayer.emplace(document());

    // 4. If el is currently render-blocking, then set request's render-blocking to true.
    // FIXME: Check if el is currently render-blocking.
    request.set_render_blocking(true);

    // 5. Return true.
    return true;
}

void HTMLLinkElement::resource_did_load_favicon()
{
    VERIFY(m_relationship & (Relationship::Icon));
    if (!resource()->has_encoded_data()) {
        dbgln_if(SPAM_DEBUG, "Favicon downloaded, no encoded data");
        return;
    }

    dbgln_if(SPAM_DEBUG, "Favicon downloaded, {} bytes from {}", resource()->encoded_data().size(), resource()->url());

    document().check_favicon_after_loading_link_resource();
}

static NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> decode_favicon(ReadonlyBytes favicon_data, URL::URL const& favicon_url, GC::Ref<DOM::Document> document)
{
    auto on_failed_decode = [favicon_url]([[maybe_unused]] Error& error) {
        dbgln_if(IMAGE_DECODER_DEBUG, "Failed to decode favicon {}: {}", favicon_url, error);
    };

    auto on_successful_decode = [document = GC::Root(document)](Web::Platform::DecodedImage& decoded_image) -> ErrorOr<void> {
        auto favicon_bitmap = decoded_image.frames[0].bitmap;
        dbgln_if(IMAGE_DECODER_DEBUG, "Decoded favicon, {}", favicon_bitmap->size());

        auto navigable = document->navigable();
        if (navigable && navigable->is_traversable())
            navigable->traversable_navigable()->page().client().page_did_change_favicon(*favicon_bitmap);

        return {};
    };

    auto promise = Platform::ImageCodecPlugin::the().decode_image(favicon_data, move(on_successful_decode), move(on_failed_decode));

    return promise;
}

bool HTMLLinkElement::load_favicon_and_use_if_window_is_active()
{
    if (!has_loaded_icon())
        return false;

    // FIXME: Refactor the caller(s) to handle the async nature of image loading
    auto promise = decode_favicon(resource()->encoded_data(), resource()->url(), document());
    auto result = promise->await();
    return !result.is_error();
}

// https://html.spec.whatwg.org/multipage/links.html#rel-icon:the-link-element-3
WebIDL::ExceptionOr<void> HTMLLinkElement::load_fallback_favicon_if_needed(GC::Ref<DOM::Document> document)
{
    auto& realm = document->realm();
    auto& vm = realm.vm();

    // In the absence of a link with the icon keyword, for Document objects whose URL's scheme is an HTTP(S) scheme,
    // user agents may instead run these steps in parallel:
    if (document->has_active_favicon())
        return {};
    if (!document->url().scheme().is_one_of("http"sv, "https"sv))
        return {};

    // 1. Let request be a new request whose URL is the URL record obtained by resolving the URL "/favicon.ico" against
    //    the Document object's URL, client is the Document object's relevant settings object, destination is "image",
    //    synchronous flag is set, credentials mode is "include", and whose use-URL-credentials flag is set.
    // NOTE: Fetch requests no longer have a synchronous flag, see https://github.com/whatwg/fetch/pull/1165
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(document->parse_url("/favicon.ico"sv));
    request->set_client(&document->relevant_settings_object());
    request->set_destination(Fetch::Infrastructure::Request::Destination::Image);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
    request->set_use_url_credentials(true);

    // 2. Let response be the result of fetching request.
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response = [document, request](GC::Ref<Fetch::Infrastructure::Response> response) {
        auto& realm = document->realm();
        auto global = GC::Ref { realm.global_object() };

        auto process_body = GC::create_function(realm.heap(), [document, request](ByteBuffer body) {
            (void)decode_favicon(body, request->url(), document);
        });
        auto process_body_error = GC::create_function(realm.heap(), [](JS::Value) {
        });

        // Check for failed favicon response
        if (!Fetch::Infrastructure::is_ok_status(response->status()) || !response->body()) {
            return;
        }

        // 3. Use response's unsafe response as an icon as if it had been declared using the icon keyword.
        if (auto body = response->unsafe_response()->body())
            body->fully_read(realm, process_body, process_body_error, global);
    };

    TRY(Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));
    return {};
}

void HTMLLinkElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_controller);
    visitor.visit(m_loaded_style_sheet);
    visitor.visit(m_rel_list);
    visitor.visit(m_sizes);
}

}
