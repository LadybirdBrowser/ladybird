/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/HTML/SharedResourceRequest.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-4/#fetch-a-style-resource
static WebIDL::ExceptionOr<GC::Ref<Fetch::Infrastructure::Request>> fetch_a_style_resource_impl(StyleResourceURL const& url_value, StyleSheetOrDocument sheet_or_document, Fetch::Infrastructure::Request::Destination destination, CorsMode cors_mode)
{
    // AD-HOC: Not every caller has a CSSStyleSheet, so allow passing a Document in instead for URL completion.
    //         Spec issue: https://github.com/w3c/csswg-drafts/issues/12065

    auto& vm = sheet_or_document.visit([](auto& it) -> JS::VM& { return it->vm(); });

    // 1. Let environmentSettings be sheet’s relevant settings object.
    auto& environment_settings = HTML::relevant_settings_object(sheet_or_document.visit([](auto& it) -> JS::Object& { return it; }));

    // 2. Let base be sheet’s stylesheet base URL if it is not null, otherwise environmentSettings’s API base URL. [CSSOM]
    // AD-HOC: We use the sheet's location if it has no base url. https://github.com/w3c/csswg-drafts/issues/12068
    auto base = sheet_or_document.visit(
        [&](GC::Ref<CSSStyleSheet> const& sheet) {
            return sheet->base_url()
                .value_or_lazy_evaluated_optional([&sheet] { return sheet->location(); })
                .value_or_lazy_evaluated([&environment_settings] { return environment_settings.api_base_url(); });
        },
        [](GC::Ref<DOM::Document> const& document) { return document->base_url(); });

    // 3. Let parsedUrl be the result of the URL parser steps with urlValue’s url and base. If the algorithm returns an error, return.
    auto url_string = url_value.visit(
        [](::URL::URL const& url) { return url.to_string(); },
        [](CSS::URL const& url) { return url.url(); });
    auto parsed_url = ::URL::Parser::basic_parse(url_string, base);
    if (!parsed_url.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::URIError, "Failed to parse URL"sv };

    // 4. Let req be a new request whose url is parsedUrl, whose destination is destination, mode is corsMode,
    //    origin is environmentSettings’s origin, credentials mode is "same-origin", use-url-credentials flag is set,
    //    client is environmentSettings, and whose referrer is environmentSettings’s API base URL.
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(parsed_url.release_value());
    request->set_destination(destination);
    request->set_mode(cors_mode == CorsMode::Cors ? Fetch::Infrastructure::Request::Mode::CORS : Fetch::Infrastructure::Request::Mode::NoCORS);
    request->set_origin(environment_settings.origin());
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);
    request->set_use_url_credentials(true);
    request->set_client(&environment_settings);
    request->set_referrer(environment_settings.api_base_url());

    // 5. If corsMode is "no-cors", set req’s credentials mode to "include".
    if (cors_mode == CorsMode::NoCors)
        request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);

    // 6. Apply any URL request modifier steps that apply to this request.
    if (auto const* css_url = url_value.get_pointer<CSS::URL>())
        apply_request_modifiers_from_url_value(*css_url, request);

    // 7. If req’s mode is "cors", set req’s referrer to sheet’s location. [CSSOM]
    if (request->mode() == Fetch::Infrastructure::Request::Mode::CORS) {
        auto location = sheet_or_document.visit(
            [](GC::Ref<CSSStyleSheet> const& sheet) { return sheet->location().value(); },
            [](GC::Ref<DOM::Document> const& document) { return document->url(); });
        request->set_referrer(move(location));
    }

    // 8. If sheet’s origin-clean flag is set, set req’s initiator type to "css". [CSSOM]
    if (auto* sheet = sheet_or_document.get_pointer<GC::Ref<CSSStyleSheet>>(); sheet && (*sheet)->is_origin_clean())
        request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::CSS);

    // 9. Fetch req, with processresponseconsumebody set to processResponse.
    // NB: Implemented by caller.
    return request;
}

// https://drafts.csswg.org/css-values-4/#fetch-a-style-resource
WebIDL::ExceptionOr<GC::Ref<Fetch::Infrastructure::FetchController>> fetch_a_style_resource(StyleResourceURL const& url_value, StyleSheetOrDocument sheet_or_document, Fetch::Infrastructure::Request::Destination destination, CorsMode cors_mode, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction process_response)
{
    auto request = TRY(fetch_a_style_resource_impl(url_value, sheet_or_document, destination, cors_mode));
    auto& environment_settings = HTML::relevant_settings_object(sheet_or_document.visit([](auto& it) -> JS::Object& { return it; }));
    auto& vm = environment_settings.vm();

    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_consume_body = move(process_response);

    return Fetch::Fetching::fetch(environment_settings.realm(), *request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));
}

// https://drafts.csswg.org/css-images-4/#fetch-an-external-image-for-a-stylesheet
GC::Ptr<HTML::SharedResourceRequest> fetch_an_external_image_for_a_stylesheet(StyleResourceURL const& url_value, StyleSheetOrDocument sheet_or_document)
{
    // To fetch an external image for a stylesheet, given a <url> url and CSSStyleSheet sheet, fetch a style resource
    // given url, with stylesheet CSSStyleSheet, destination "image", CORS mode "no-cors", and processResponse being
    // the following steps given response res and null, failure or a byte stream byteStream: If byteStream is a byte
    // stream, load the image from the byte stream.

    // NB: We can't directly call fetch_a_style_resource() because we want to make use of SharedResourceRequest to
    //     deduplicate image requests.

    auto maybe_request = fetch_a_style_resource_impl(url_value, sheet_or_document, Fetch::Infrastructure::Request::Destination::Image, CorsMode::NoCors);
    if (maybe_request.is_error())
        return nullptr;
    auto& request = maybe_request.value();

    auto document = sheet_or_document.visit(
        [&](GC::Ref<CSSStyleSheet> const& sheet) -> GC::Ref<DOM::Document> { return *sheet->owning_document(); },
        [](GC::Ref<DOM::Document> const& document) -> GC::Ref<DOM::Document> { return document; });
    auto& realm = document->realm();

    auto shared_resource_request = HTML::SharedResourceRequest::get_or_create(realm, document->page(), request->url());
    shared_resource_request->add_callbacks(
        [document, weak_document = document->make_weak_ptr<DOM::Document>()] {
            if (!weak_document)
                return;

            if (auto navigable = document->navigable()) {
                // Once the image has loaded, we need to re-resolve CSS properties that depend on the image's dimensions.
                document->set_needs_to_resolve_paint_only_properties();

                // FIXME: Do less than a full repaint if possible?
                document->set_needs_display();
            }
        },
        nullptr);

    if (shared_resource_request->needs_fetching())
        shared_resource_request->fetch_resource(realm, *request);

    return shared_resource_request;
}

// https://drafts.csswg.org/css-values-5/#apply-request-modifiers-from-url-value
void apply_request_modifiers_from_url_value(URL const& url, GC::Ref<Fetch::Infrastructure::Request> request)
{
    // To apply request modifiers from URL value given a request req and a <url> url, call the URL request modifier
    // steps for url’s <request-url-modifier>s in sequence given req.
    for (auto const& request_url_modifier : url.request_url_modifiers())
        request_url_modifier.modify_request(request);
}

}
