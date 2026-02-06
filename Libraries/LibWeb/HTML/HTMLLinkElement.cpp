/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/HTMLLinkElementPrototype.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLLinkElement);

HTMLLinkElement::HTMLLinkElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLLinkElement::~HTMLLinkElement() = default;

void HTMLLinkElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLLinkElement);
    Base::initialize(realm);
}

void HTMLLinkElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_controller);
    visitor.visit(m_loaded_style_sheet);
    visitor.visit(m_rel_list);
    visitor.visit(m_sizes);
}

void HTMLLinkElement::inserted()
{
    HTMLElement::inserted();

    if (!document().browsing_context())
        return;

    if (should_fetch_and_process_resource_type() && is_browsing_context_connected()) {
        // The appropriate times to fetch and process this type of link are:
        //  - When the external resource link is created on a link element that is already browsing-context connected.
        //  - When the external resource link's link element becomes browsing-context connected.
        fetch_and_process_linked_resource();
    }
}

void HTMLLinkElement::removed_from(Node* old_parent, Node& old_root)
{
    Base::removed_from(old_parent, old_root);

    if (m_loaded_style_sheet) {
        auto& style_sheet_list = [&old_root] -> CSS::StyleSheetList& {
            if (auto* shadow_root = as_if<DOM::ShadowRoot>(old_root); shadow_root)
                return shadow_root->style_sheets();

            return as<DOM::Document>(old_root).style_sheets();
        }();

        style_sheet_list.remove_a_css_style_sheet(*m_loaded_style_sheet);
        m_loaded_style_sheet = nullptr;
    }
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

void HTMLLinkElement::set_media(String media)
{
    set_attribute_value(HTML::AttributeNames::media, media);
    if (auto sheet = m_loaded_style_sheet)
        sheet->set_media(move(media));
}

String HTMLLinkElement::media() const
{
    return attribute(HTML::AttributeNames::media).value_or(String {});
}

// https://drafts.csswg.org/cssom/#dom-linkstyle-sheet
GC::Ptr<CSS::CSSStyleSheet> HTMLLinkElement::sheet() const
{
    return m_loaded_style_sheet;
}

void HTMLLinkElement::finished_loading_critical_style_subresources(AnyFailed)
{
    m_document_load_event_delayer.clear();
}

bool HTMLLinkElement::has_loaded_icon() const
{
    return m_relationship & Relationship::Icon && m_loaded_icon.has_value();
}

void HTMLLinkElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // https://html.spec.whatwg.org/multipage/semantics.html#processing-the-type-attribute:attr-link-type
    if (name == HTML::AttributeNames::type) {
        if (value.has_value())
            m_mime_type = value->to_ascii_lowercase();
        else {
            m_mime_type = {};
        }

        return;
    }

    // 4.6.7 Link types - https://html.spec.whatwg.org/multipage/links.html#linkTypes
    auto old_relationship = m_relationship;
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

    if ((m_relationship & Relationship::Stylesheet) && m_loaded_style_sheet) {
        if (name == HTML::AttributeNames::disabled) {
            document_or_shadow_root_style_sheets().remove_a_css_style_sheet(*m_loaded_style_sheet);
            m_loaded_style_sheet = nullptr;
        } else if (name == HTML::AttributeNames::media) {
            m_loaded_style_sheet->set_media(value.value_or(String {}));
        }
    }

    if (should_fetch_and_process_resource_type() && is_browsing_context_connected()) {
        // The appropriate times to fetch and process this type of link are:
        // - When the href attribute of the link element of an external resource link that is already browsing-context connected is changed.
        auto fetch = name == AttributeNames::href;

        if (!fetch && (m_relationship & (Relationship::Preconnect | Relationship::Stylesheet))) {
            // - When the crossorigin attribute of the link element of an external resource link that is already browsing-context connected is set, changed, or removed.
            fetch = name == AttributeNames::crossorigin;
        }

        if (!fetch && (m_relationship & Relationship::Preload)) {
            fetch =
                // - When the as attribute of the link element of an external resource link that is already browsing-context connected is changed.
                name == AttributeNames::as

                // FIXME: - When the type attribute of the link element of an external resource link that is already browsing-context connected, but was previously not obtained due to the type attribute specifying an unsupported type for the request destination, is set, removed, or changed.
                // FIXME: - When the media attribute of the link element of an external resource link that is already browsing-context connected, but was previously not obtained due to the media attribute not matching the environment, is changed or removed.
                ;
        }

        if (!fetch && (m_relationship & Relationship::Stylesheet)) {
            fetch =
                // - When the disabled attribute of the link element of an external resource link that is already browsing-context connected is set, changed, or removed.
                name == AttributeNames::disabled

                // FIXME: - When the type attribute of the link element of an external resource link that is already browsing-context connected is set or changed to a value that does not or no longer matches the Content-Type metadata of the previous obtained external resource, if any.
                // FIXME: - When the type attribute of the link element of an external resource link that is already browsing-context connected, but was previously not obtained due to the type attribute specifying an unsupported type, is removed or changed.

                // AD-HOC: When the link element's type becomes a stylesheet
                || !(old_relationship & Relationship::Stylesheet);
        }

        if (fetch)
            fetch_and_process_linked_resource();
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#contributes-a-script-blocking-style-sheet
bool HTMLLinkElement::contributes_a_script_blocking_style_sheet() const
{
    // An element el in the context of a Document of an HTML parser or XML parser
    // contributes a script-blocking style sheet if all of the following are true:

    // el was created by that Document's parser.
    if (m_parser_document != &document())
        return false;

    // FIXME: el is either a style element or a link element that was an external resource link that contributes to the styling processing model when the el was created by the parser.

    // FIXME: el's media attribute's value matches the environment.

    // el's style sheet was enabled when the element was created by the parser.
    if (!m_was_enabled_when_created_by_parser)
        return false;

    // FIXME: The last time the event loop reached step 1, el's root was that Document.

    // The user agent hasn't given up on loading that particular style sheet yet.
    // A user agent may give up on loading a style sheet at any time.
    if (m_fetch_controller && m_fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Terminated)
        return false;
    if (m_fetch_controller && m_fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Aborted)
        return false;

    return true;
}

bool HTMLLinkElement::is_implicitly_potentially_render_blocking() const
{
    // A link element of this type is implicitly potentially render-blocking if the element was created by its node document's parser.
    return &document() == m_parser_document;
}

// https://html.spec.whatwg.org/multipage/semantics.html#create-link-options-from-element
GC::Ref<HTMLLinkElement::LinkProcessingOptions> HTMLLinkElement::create_link_options()
{
    // 1. Let document be el's node document.
    auto& document = this->document();

    // 2. Let options be a new link processing options with
    auto options = realm().create<LinkProcessingOptions>(
        // crossorigin
        //     the state of el's crossorigin content attribute
        cors_setting_attribute_from_keyword(get_attribute(AttributeNames::crossorigin)),

        // referrer policy
        //     the state of el's referrerpolicy content attribute
        ReferrerPolicy::from_string(get_attribute(AttributeNames::referrerpolicy).value_or({})).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString),

        // FIXME: source set
        //     el's source set

        // base URL
        //     document's document base URL
        document.base_url(),

        // origin
        //     document's origin
        document.origin(),

        // environment
        //     document's relevant settings object
        document.relevant_settings_object(),

        // policy container
        //     document's policy container
        document.policy_container(),

        // document
        //     document
        document,

        // cryptographic nonce metadata
        //     the current value of el's [[CryptographicNonce]] internal slot
        m_cryptographic_nonce,

        // fetch priority
        //     the state of el's fetchpriority content attribute
        Fetch::Infrastructure::request_priority_from_string(get_attribute_value(HTML::AttributeNames::fetchpriority)).value_or(Fetch::Infrastructure::Request::Priority::Auto));

    // 3. If el has an href attribute, then set options's href to the value of el's href attribute.
    if (auto maybe_href = get_attribute(AttributeNames::href); maybe_href.has_value())
        options->href = maybe_href.value();

    // 4. If el has an integrity attribute, then set options's integrity to the value of el's integrity content attribute.
    if (auto maybe_integrity = get_attribute(AttributeNames::integrity); maybe_integrity.has_value())
        options->integrity = maybe_integrity.value();

    // 5. If el has a type attribute, then set options's type to the value of el's type attribute.
    if (auto maybe_type = get_attribute(AttributeNames::type); maybe_type.has_value())
        options->type = maybe_type.value();

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
    // FIXME: Spec issue: We should be parsing this URL relative to a document or environment settings object.
    //        https://github.com/whatwg/html/issues/9715
    auto url = DOMURL::parse(options.href, options.base_url);

    // 4. If url is failure, then return null.
    if (!url.has_value())
        return nullptr;

    // 5. Let request be the result of creating a potential-CORS request given url, options's destination, and options's crossorigin.
    auto request = create_potential_CORS_request(vm(), *url, options.destination, options.crossorigin);

    // 6. Set request's policy container to options's policy container.
    request->set_policy_container(GC::Ref { *options.policy_container });

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
    if (m_relationship & ~(Relationship::DNSPrefetch | Relationship::Preconnect | Relationship::Preload))
        default_fetch_and_process_linked_resource();
    else if (m_relationship & Relationship::Preload)
        fetch_and_process_linked_preload_resource();
    else if (m_relationship & Relationship::Preconnect)
        fetch_and_process_linked_preconnect_resource();
    else if (m_relationship & Relationship::DNSPrefetch)
        fetch_and_process_linked_dns_prefetch_resource();
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
    fetch_algorithms_input.process_response_consume_body = [this](auto response, auto body_bytes) {
        // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
        //        https://github.com/whatwg/html/issues/9355
        response = response->unsafe_response();

        // 1. Let success be true.
        bool success = true;
        ByteBuffer successful_body_bytes;

        // 2. If either of the following conditions are met:
        // - bodyBytes is null or failure; or
        // - response's status is not an ok status,
        // then set success to false.
        body_bytes.visit(
            [&](ByteBuffer& body_bytes) {
                if (Fetch::Infrastructure::is_ok_status(response->status()))
                    successful_body_bytes = move(body_bytes);
                else
                    success = false;
            },
            [&](auto) { success = false; });

        // FIXME: 3. Otherwise, wait for the link resource's critical subresources to finish loading.

        // 4. Process the linked resource given el, success, response, and bodyBytes.
        process_linked_resource(success, response, move(successful_body_bytes));
    };

    if (m_fetch_controller)
        m_fetch_controller->abort(realm(), {});
    m_fetch_controller = Fetch::Fetching::fetch(realm(), *request, Fetch::Infrastructure::FetchAlgorithms::create(vm(), move(fetch_algorithms_input)));
}

// https://html.spec.whatwg.org/multipage/links.html#link-type-dns-prefetch:fetch-and-process-the-linked-resource-2
void HTMLLinkElement::fetch_and_process_linked_dns_prefetch_resource()
{
    auto href = get_attribute(AttributeNames::href);
    if (!href.has_value())
        return;

    // 1. Let url be the result of encoding-parsing a URL given el's href attribute's value, relative to el's node document.
    auto url = document().encoding_parse_url(*href);

    // 2. If url is failure, then return.
    if (!url.has_value())
        return;

    // FIXME: 3. Let partitionKey be the result of determining the network partition key given el's node document's relevant
    //           settings object.

    // 4. The user agent should resolve an origin given partitionKey and url's origin.
    // FIXME: This should go through Fetch: https://fetch.spec.whatwg.org/#resolve-an-origin
    ResourceLoader::the().prefetch_dns(url.value());
}

// https://html.spec.whatwg.org/multipage/links.html#link-type-preconnect:fetch-and-process-the-linked-resource-2
void HTMLLinkElement::fetch_and_process_linked_preconnect_resource()
{
    // The fetch and process the linked resource steps for this type of linked resource, given a link element el, are to
    // create link options from el and to preconnect given the result.
    preconnect(create_link_options());
}

// https://html.spec.whatwg.org/multipage/links.html#translate-a-preload-destination
static Variant<Empty, Optional<Fetch::Infrastructure::Request::Destination>> translate_a_preload_destination(Optional<String> const& destination)
{
    // 1. If destination is not "fetch", "font", "image", "script", "style", or "track", then return null.
    if (!destination.has_value() || !destination->is_one_of("fetch"sv, "font"sv, "image"sv, "script"sv, "style"sv, "track"sv))
        return {};

    // 2. Return the result of translating destination.
    return Fetch::Infrastructure::translate_potential_destination(*destination);
}

// https://html.spec.whatwg.org/multipage/links.html#link-type-preload:fetch-and-process-the-linked-resource-2
void HTMLLinkElement::fetch_and_process_linked_preload_resource()
{
    // FIXME: 1. Update the source set for el.

    // 2. Let options be the result of creating link options from el.
    auto options = create_link_options();

    // 3. Let destination be the result of translating the keyword representing the state of el's as attribute.
    auto destination = translate_a_preload_destination(get_attribute(HTML::AttributeNames::as));

    // 4. If destination is null, then return.
    if (destination.has<Empty>())
        return;

    // 5. Set options's destination to destination.
    options->destination = destination.get<Optional<Fetch::Infrastructure::Request::Destination>>();

    // 6. Preload options, with the following steps given a response response:
    preload(options, GC::Function<void(Fetch::Infrastructure::Response&)>::create(heap(), [this](Fetch::Infrastructure::Response& response) {
        // 1. If response is a network error, fire an event named error at el. Otherwise, fire an event named load at el.
        if (response.is_network_error())
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
        else
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));
    }));
}

// https://html.spec.whatwg.org/multipage/semantics.html#linked-resource-fetch-setup-steps
bool HTMLLinkElement::linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request& request)
{
    if (m_relationship & Relationship::Icon)
        return icon_linked_resource_fetch_setup_steps(request);
    if (m_relationship & Relationship::Stylesheet)
        return stylesheet_linked_resource_fetch_setup_steps(request);
    return true;
}

// https://html.spec.whatwg.org/multipage/links.html#rel-icon:linked-resource-fetch-setup-steps
bool HTMLLinkElement::icon_linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request& request)
{
    // 1. Set request's destination to "image".
    request.set_destination(Fetch::Infrastructure::Request::Destination::Image);

    // 2. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:linked-resource-fetch-setup-steps
bool HTMLLinkElement::stylesheet_linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request& request)
{
    // 1. If el's disabled attribute is set, then return false.
    if (has_attribute(AttributeNames::disabled))
        return false;

    // 2. If el contributes a script-blocking style sheet, append el to its node document's script-blocking style sheet set.
    if (contributes_a_script_blocking_style_sheet())
        document().script_blocking_style_sheet_set().set(*this);

    // 3. If el's media attribute's value matches the environment and el is potentially render-blocking, then block rendering on el.
    // FIXME: Check media attribute value.
    if (is_potentially_render_blocking())
        block_rendering();

    m_document_load_event_delayer.emplace(document());

    // 4. If el is currently render-blocking, then set request's render-blocking to true.
    if (document().is_render_blocking_element(*this))
        request.set_render_blocking(true);

    // FIXME: We currently don't set the destination for stylesheets, so we do it here.
    //        File a spec issue that the destination for stylesheets is not actually set if the `as` attribute is missing.
    request.set_destination(Fetch::Infrastructure::Request::Destination::Style);

    // 5. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/links.html#preconnect
void HTMLLinkElement::preconnect(LinkProcessingOptions const& options)
{
    // 1. If options's href is an empty string, return.
    if (options.href.is_empty())
        return;

    // 2. Let url be the result of encoding-parsing a URL given options's href, relative to options's base URL.
    // FIXME: Spec issue: We should be parsing this URL relative to a document or environment settings object.
    //        https://github.com/whatwg/html/issues/9715
    auto url = DOMURL::parse(options.href, options.base_url);

    // 3. If url is failure, then return.
    if (!url.has_value())
        return;

    // 4. If url's scheme is not an HTTP(S) scheme, then return.
    if (!url->scheme().is_one_of("http"sv, "https"sv))
        return;

    // FIXME: 5. Let partitionKey be the result of determining the network partition key given options's environment.
    // FIXME: 6. Let useCredentials be true.
    // FIXME: 7. If options's crossorigin is Anonymous and options's origin does not have the same origin as url's origin,
    //           then set useCredentials to false.

    // 8. The user agent should obtain a connection given partitionKey, url's origin, and useCredentials.
    // FIXME: This should go through Fetch: https://fetch.spec.whatwg.org/#concept-connection-obtain
    ResourceLoader::the().preconnect(*url);
}

// https://html.spec.whatwg.org/multipage/links.html#match-preload-type
static bool type_matches_destination(StringView type, Optional<Fetch::Infrastructure::Request::Destination> destination)
{
    using enum Fetch::Infrastructure::Request::Destination;

    // 1. If type is an empty string, then return true.
    if (type.is_empty())
        return true;

    // 2. If destination is "fetch", then return true.
    // FIXME: Spec issue: "fetch" will have been turned to an empty string by this point.
    if (!destination.has_value())
        return true;

    // 3. Let mimeTypeRecord be the result of parsing type.
    auto mime_type_record = MimeSniff::MimeType::parse(type);

    // 4. If mimeTypeRecord is failure, then return false.
    if (!mime_type_record.has_value())
        return false;

    // FIXME: 5. If mimeTypeRecord is not supported by the user agent, then return false.

    // 6. If any of the following are true:
    if (
        // destination is "audio" or "video", and mimeTypeRecord is an audio or video MIME type;
        ((destination == Audio || destination == Video) && mime_type_record->is_audio_or_video())

        // destination is a script-like destination and mimeTypeRecord is a JavaScript MIME type;
        || (Fetch::Infrastructure::destination_is_script_like(*destination) && mime_type_record->is_javascript())

        // destination is "image" and mimeTypeRecord is an image MIME type;
        || (destination == Image && mime_type_record->is_image())

        // destination is "font" and mimeTypeRecord is a font MIME type;
        || (destination == Font && mime_type_record->is_font())

        // destination is "json" and mimeTypeRecord is a JSON MIME type;
        || (destination == JSON && mime_type_record->is_json())

        // destination is "style" and mimeTypeRecord's essence is text/css; or
        || (destination == Style && mime_type_record->essence() == "text/css"sv)

        // destination is "track" and mimeTypeRecord's essence is text/vtt,
        || (destination == Track && mime_type_record->essence() == "text/vtt"sv)) {
        // then return true.
        return true;
    }

    // 7. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/links.html#preload
void HTMLLinkElement::preload(LinkProcessingOptions& options, GC::Ptr<GC::Function<void(Fetch::Infrastructure::Response&)>> process_response)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. If options's type doesn't match options's destination, then return.
    if (!type_matches_destination(options.type, options.destination))
        return;

    // FIXME: 2. If options's destination is "image" and options's source set is not null, then set options's href to the
    //           result of selecting an image source from options's source set.
    if (options.href.is_empty())
        return;

    // 3. Let request be the result of creating a link request given options.
    auto request = create_link_request(options);

    // 4. If request is null, then return.
    if (!request)
        return;

    // FIXME: 5. Let unsafeEndTime be 0.

    // 6. Let entry be a new preload entry whose integrity metadata is options's integrity.
    auto entry = realm.create<PreloadEntry>();
    entry->integrity_metadata = options.integrity;

    // 7. Let key be the result of creating a preload key given request.
    auto key = PreloadKey::create(*request);

    // 8. If options's document is null, then set request's initiator type to "early hint".
    if (!options.document)
        request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::EarlyHint);

    // 9. Let controller be null.
    auto controller_holder = Fetch::Infrastructure::FetchControllerHolder::create(vm);

    // 10. Let reportTiming given a Document document be to report timing for controller given document's relevant global object.
    auto report_timing = GC::Function<void(DOM::Document const&)>::create(realm.heap(), [controller_holder](DOM::Document const& document) {
        controller_holder->controller()->report_timing(relevant_global_object(document));
    });

    // 11. Set controller to the result of fetching request, with processResponseConsumeBody set to the following steps
    //     given a response response and null, failure, or a byte sequence bodyBytes:
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_consume_body = [&realm, options = GC::Ref { options }, process_response, entry, report_timing](GC::Ref<Fetch::Infrastructure::Response> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
        // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
        //        https://github.com/whatwg/html/issues/9355
        response = response->unsafe_response();

        // 1. If bodyBytes is a byte sequence, then set response's body to bodyBytes as a body.
        if (auto* byte_sequence = body_bytes.get_pointer<ByteBuffer>())
            response->set_body(Fetch::Infrastructure::byte_sequence_as_body(realm, *byte_sequence));
        // 2. Otherwise, set response to a network error.
        else
            response = Fetch::Infrastructure::Response::network_error(realm.vm(), "Expected preload response to contain a body"_string);

        // FIXME: 3. Set unsafeEndTime to the unsafe shared current time.

        // 4. If options's document is not null, then call reportTiming given options's document.
        if (options->document)
            report_timing->function()(*options->document);

        // 5. If entry's on response available is null, then set entry's response to response; otherwise call entry's
        //    on response available given response.
        if (!entry->on_response_available)
            entry->response = response;
        else
            entry->on_response_available->function()(response);

        // 6. If processResponse is given, then call processResponse with response.
        if (process_response)
            process_response->function()(response);
    };

    m_fetch_controller = Fetch::Fetching::fetch(realm, *request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));
    controller_holder->set_controller(*m_fetch_controller);

    // 12. Let commit be the following steps given a Document document:
    auto commit = GC::Function<void(DOM::Document&)>::create(realm.heap(), [entry, report_timing](DOM::Document& document) {
        // 1. If entry's response is not null, then call reportTiming given document.
        if (entry->response)
            report_timing->function()(document);

        // FIXME: 2. Set document's map of preloaded resources[key] to entry.
    });

    // 13. If options's document is null, then set options's on document ready to commit. Otherwise, call commit with
    //     options's document.
    if (!options.document)
        options.on_document_ready = commit;
    else
        commit->function()(*options.document);
}

// https://html.spec.whatwg.org/multipage/semantics.html#process-the-linked-resource
void HTMLLinkElement::process_linked_resource(bool success, Fetch::Infrastructure::Response const& response, ByteBuffer body_bytes)
{
    if (m_relationship & Relationship::Icon)
        process_icon_resource(success, response, move(body_bytes));
    else if (m_relationship & Relationship::Stylesheet)
        process_stylesheet_resource(success, response, move(body_bytes));
}

// AD-HOC: The spec is underspecified for fetching and processing rel="icon" See:
//         https://github.com/whatwg/html/issues/1769
void HTMLLinkElement::process_icon_resource(bool success, Fetch::Infrastructure::Response const& response, ByteBuffer body_bytes)
{
    if (!success)
        return;

    m_loaded_icon = { response.url().value_or({}), move(body_bytes) };
    document().check_favicon_after_loading_link_resource();
}

// https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:process-the-linked-resource
void HTMLLinkElement::process_stylesheet_resource(bool success, Fetch::Infrastructure::Response const& response, ByteBuffer body_bytes)
{
    if (!document().is_fully_active())
        return;

    // 1. If the resource's Content-Type metadata is not text/css, then set success to false.
    auto mime_type_string = m_mime_type;
    Optional<String> mime_type_charset;
    auto extracted_mime_type = Fetch::Infrastructure::extract_mime_type(response.header_list());
    if (extracted_mime_type.has_value()) {
        if (!mime_type_string.has_value())
            mime_type_string = extracted_mime_type->essence();
        if (auto charset = extracted_mime_type->parameters().get("charset"sv); charset.has_value())
            mime_type_charset = charset.value();
    }

    if (mime_type_string.has_value() && mime_type_string != "text/css"sv)
        success = false;

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
        Optional<StringView> environment_encoding;
        if (auto charset = attribute(HTML::AttributeNames::charset); charset.has_value())
            environment_encoding = TextCodec::get_standardized_encoding(charset.release_value());

        if (!environment_encoding.has_value() && document().encoding().has_value())
            environment_encoding = document().encoding().value();

        auto maybe_decoded_string = css_decode_bytes(environment_encoding, mime_type_charset, body_bytes);
        if (maybe_decoded_string.is_error()) {
            dbgln("Failed to decode CSS file: {}", response.url().value_or(URL::URL()));
            dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::error));
        } else {
            VERIFY(!response.url_list().is_empty());
            m_loaded_style_sheet = document_or_shadow_root_style_sheets().create_a_css_style_sheet(
                maybe_decoded_string.release_value(),
                "text/css"_string,
                this,
                attribute(HTML::AttributeNames::media).value_or({}),
                in_a_document_tree() ? attribute(HTML::AttributeNames::title).value_or({}) : String {},
                (m_relationship & Relationship::Alternate && !m_explicitly_enabled) ? CSS::StyleSheetList::Alternate::Yes : CSS::StyleSheetList::Alternate::No,
                CSS::StyleSheetList::OriginClean::Yes,
                response.url_list().first(),
                nullptr,
                nullptr);

            // 2. Fire an event named load at el.
            dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::load));
        }
    }
    // 5. Otherwise, fire an event named error at el.
    else {
        dispatch_event(*DOM::Event::create(realm(), HTML::EventNames::error));
    }

    // 6. If el contributes a script-blocking style sheet, then:
    if (contributes_a_script_blocking_style_sheet()) {
        // 1. Assert: el's node document's script-blocking style sheet set contains el.
        VERIFY(document().script_blocking_style_sheet_set().contains(*this));

        // 2. Remove el from its node document's script-blocking style sheet set.
        document().script_blocking_style_sheet_set().remove(*this);
    }

    // 7. Unblock rendering on el.
    unblock_rendering();

    if (m_loaded_style_sheet) {
        auto style_sheet_loading_state = m_loaded_style_sheet->loading_state();
        if (style_sheet_loading_state == CSS::CSSStyleSheet::LoadingState::Loaded || style_sheet_loading_state == CSS::CSSStyleSheet::LoadingState::Error) {
            finished_loading_critical_style_subresources(style_sheet_loading_state == CSS::CSSStyleSheet::LoadingState::Error ? AnyFailed::Yes : AnyFailed::No);
        }
    } else {
        m_document_load_event_delayer.clear();
    }
}

static NonnullRefPtr<Core::Promise<bool>> decode_favicon(ReadonlyBytes favicon_data, URL::URL const& favicon_url, GC::Ref<DOM::Document> document)
{
    auto promise = Core::Promise<bool>::construct();

    if (favicon_url.basename().ends_with(".svg"sv)) {
        auto result = SVG::SVGDecodedImageData::create(document->realm(), document->page(), favicon_url, favicon_data);
        if (result.is_error()) {
            promise->reject(Error::from_string_view("Failed to decode SVG favicon"sv));
            return promise;
        }

        // FIXME: Calculate size based on device pixel ratio
        Gfx::IntSize size { 32, 32 };
        auto immutable_bitmap = result.release_value()->bitmap(0, size);
        if (!immutable_bitmap) {
            promise->reject(Error::from_string_view("Failed to get bitmap from SVG favicon"sv));
            return promise;
        }
        auto bitmap = immutable_bitmap->bitmap();
        auto navigable = document->navigable();
        if (navigable && navigable->is_traversable())
            navigable->traversable_navigable()->page().client().page_did_change_favicon(*bitmap);
        promise->resolve(true);
        return promise;
    }

    auto on_failed_decode = [favicon_url, promise]([[maybe_unused]] Error& error) {
        dbgln_if(IMAGE_DECODER_DEBUG, "Failed to decode favicon {}: {}", favicon_url, error);
        promise->reject(move(error));
    };

    auto on_successful_decode = [document = GC::Root(document), promise](Web::Platform::DecodedImage& decoded_image) -> ErrorOr<void> {
        auto favicon_bitmap = decoded_image.frames[0].bitmap;
        dbgln_if(IMAGE_DECODER_DEBUG, "Decoded favicon, {}", favicon_bitmap->size());

        auto navigable = document->navigable();
        if (navigable && navigable->is_traversable())
            navigable->traversable_navigable()->page().client().page_did_change_favicon(*favicon_bitmap);

        promise->resolve(true);
        return {};
    };

    (void)Platform::ImageCodecPlugin::the().decode_image(favicon_data, move(on_successful_decode), move(on_failed_decode));

    return promise;
}

bool HTMLLinkElement::load_favicon_and_use_if_window_is_active()
{
    if (!has_loaded_icon())
        return false;

    // FIXME: Refactor the caller(s) to handle the async nature of image loading
    auto promise = decode_favicon(m_loaded_icon->icon, m_loaded_icon->url, document());
    auto result = promise->await();
    return !result.is_error();
}

// https://html.spec.whatwg.org/multipage/links.html#rel-icon:the-link-element-3
void HTMLLinkElement::load_fallback_favicon_if_needed(GC::Ref<DOM::Document> document)
{
    auto& realm = document->realm();
    auto& vm = realm.vm();

    // In the absence of a link with the icon keyword, for Document objects whose URL's scheme is an HTTP(S) scheme,
    // user agents may instead run these steps in parallel:
    if (document->has_active_favicon())
        return;
    if (!document->url().scheme().is_one_of("http"sv, "https"sv))
        return;

    // AD-HOC: Don't load fallback favicon for auxiliary browsing contexts (popup windows).
    // This matches the behavior observed in Chrome and Firefox, and avoids unnecessary network requests
    // that can interfere with Content Security Policy violation reporting.
    // See: https://github.com/whatwg/html/issues/12082
    if (auto browsing_context = document->browsing_context(); browsing_context->is_auxiliary())
        return;

    // 1. Let request be a new request whose URL is the URL record obtained by resolving the URL "/favicon.ico" against
    //    the Document object's URL, client is the Document object's relevant settings object, destination is "image",
    //    synchronous flag is set, credentials mode is "include", and whose use-URL-credentials flag is set.
    // NOTE: Fetch requests no longer have a synchronous flag, see https://github.com/whatwg/fetch/pull/1165
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(*document->encoding_parse_url("/favicon.ico"sv));
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

    Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));
}

bool HTMLLinkElement::should_fetch_and_process_resource_type() const
{
    // https://html.spec.whatwg.org/multipage/links.html#link-type-dns-prefetch:fetch-and-process-the-linked-resource
    // https://html.spec.whatwg.org/multipage/links.html#link-type-preconnect:fetch-and-process-the-linked-resource
    // https://html.spec.whatwg.org/multipage/links.html#link-type-preload:fetch-and-process-the-linked-resource
    // https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:fetch-and-process-the-linked-resource
    if (m_relationship & (Relationship::DNSPrefetch | Relationship::Preconnect | Relationship::Preload | Relationship::Stylesheet))
        return true;

    // AD-HOC: The spec is underspecified for fetching and processing rel="icon". See:
    //         https://github.com/whatwg/html/issues/1769
    return m_relationship & Relationship::Icon;
}

HTMLLinkElement::LinkProcessingOptions::LinkProcessingOptions(
    CORSSettingAttribute crossorigin,
    ReferrerPolicy::ReferrerPolicy referrer_policy,
    URL::URL base_url,
    URL::Origin origin,
    GC::Ref<HTML::EnvironmentSettingsObject> environment,
    GC::Ref<HTML::PolicyContainer> policy_container,
    GC::Ptr<Web::DOM::Document> document,
    String cryptographic_nonce_metadata,
    Fetch::Infrastructure::Request::Priority fetch_priority)
    : cryptographic_nonce_metadata(move(cryptographic_nonce_metadata))
    , crossorigin(crossorigin)
    , referrer_policy(referrer_policy)
    , base_url(move(base_url))
    , origin(move(origin))
    , environment(environment)
    , policy_container(policy_container)
    , document(document)
    , fetch_priority(fetch_priority)
{
}

void HTMLLinkElement::LinkProcessingOptions::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(environment);
    visitor.visit(policy_container);
    visitor.visit(document);
    visitor.visit(on_document_ready);
}

// https://html.spec.whatwg.org/multipage/links.html#create-a-preload-key
HTMLLinkElement::PreloadKey HTMLLinkElement::PreloadKey::create(Fetch::Infrastructure::Request const& request)
{
    // To create a preload key for a request request, return a new preload key whose URL is request's URL, destination
    // is request's destination, mode is request's mode, and credentials mode is request's credentials mode.
    return PreloadKey {
        .url = request.url(),
        .destination = request.destination(),
        .mode = request.mode(),
        .credentials_mode = request.credentials_mode(),
    };
}

void HTMLLinkElement::PreloadEntry::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(response);
    visitor.visit(on_response_available);
}

GC_DEFINE_ALLOCATOR(HTMLLinkElement::LinkProcessingOptions);
GC_DEFINE_ALLOCATOR(HTMLLinkElement::PreloadEntry);

}
