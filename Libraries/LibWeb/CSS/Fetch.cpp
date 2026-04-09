/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-4/#style-resource-base-url

struct StyleSheetAndURL {
    GC::Ptr<CSSStyleSheet> sheet;
    ::URL::URL url;
};
static StyleSheetAndURL style_resource_base_url(RuleOrDeclaration css_rule_or_declaration)
{
    // 1. Let sheet be null.
    GC::Ptr<CSSStyleSheet> sheet;

    // 2. If cssRuleOrDeclaration is a CSS declaration block whose parent CSS rule is not null, set cssRuleOrDeclaration to cssRuleOrDeclaration’s parent CSS rule.
    if (auto* block = css_rule_or_declaration.value.get_pointer<RuleOrDeclaration::StyleDeclaration>()) {
        if (block->parent_rule)
            css_rule_or_declaration.value = RuleOrDeclaration::Rule { block->parent_rule->parent_style_sheet() };
    }

    // 3. If cssRuleOrDeclaration is a CSS rule, set sheet to cssRuleOrDeclaration’s parent style sheet.
    if (auto* rule = css_rule_or_declaration.value.get_pointer<RuleOrDeclaration::Rule>()) {
        if (rule->parent_style_sheet) {
            sheet = rule->parent_style_sheet;
        }
    }

    // 4. If sheet is not null:
    if (sheet) {
        // 1. If sheet’s stylesheet base URL is not null, return sheet’s stylesheet base URL.
        if (auto base_url = sheet->base_url(); base_url.has_value())
            return { sheet, base_url.value() };

        // 2. If sheet’s location is not null, return sheet’s location.
        if (auto location = sheet->location(); location.has_value())
            return { sheet, location.value() };
    }

    // 5. Return cssRuleOrDeclaration’s relevant settings object’s API base URL.
    return { sheet, css_rule_or_declaration.environment_settings_object->api_base_url() };
}

// https://drafts.csswg.org/css-values-4/#resolve-a-style-resource-url
static Optional<::URL::URL> resolve_a_style_resource_url(StyleResourceURL const& url_value, RuleOrDeclaration css_rule_or_declaration)
{
    // 1. Let baseURL be the style resource base URL given cssRuleOrDeclaration.
    auto [_, base_url] = style_resource_base_url(css_rule_or_declaration);

    // 2. Return the result of the URL parser steps with urlValue’s url and base.
    auto url_string = url_value.visit(
        [](::URL::URL const& url) { return url.to_string(); },
        [](CSS::URL const& url) { return url.url(); });
    return DOMURL::parse(url_string, base_url);
}

// https://drafts.csswg.org/css-values-4/#fetch-a-style-resource
static GC::Ptr<Fetch::Infrastructure::Request> fetch_a_style_resource_impl(StyleResourceURL const& url_value, RuleOrDeclaration css_rule_or_declaration, Fetch::Infrastructure::Request::Destination destination, CorsMode cors_mode)
{
    auto& vm = css_rule_or_declaration.environment_settings_object->vm();

    // 1. Let parsedUrl be the result of resolving urlValue given cssRuleOrDeclaration. If that failed, return.
    auto parsed_url = resolve_a_style_resource_url(url_value, css_rule_or_declaration);
    if (!parsed_url.has_value())
        return {};

    // 2. Let settingsObject be cssRuleOrDeclaration’s relevant settings object.
    auto& environment_settings = *css_rule_or_declaration.environment_settings_object;

    // 3. Let req be a new request whose url is parsedUrl, whose destination is destination, mode is corsMode,
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

    // 4. If corsMode is "no-cors", set req’s credentials mode to "include".
    if (cors_mode == CorsMode::NoCors)
        request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);

    // 5. Apply any URL request modifier steps that apply to this request.
    if (auto const* css_url = url_value.get_pointer<CSS::URL>())
        apply_request_modifiers_from_url_value(*css_url, request);

    // 6. If req’s mode is "cors", and sheet is not null, then set req’s referrer to the style resource base URL given cssRuleOrDeclaration. [CSSOM]
    // FIXME: Spec issue - sheet is not defined as a variable, we use the sheet determined from 'style resource base URL' instead.
    //        https://github.com/w3c/csswg-drafts/issues/12288
    auto [sheet, base_url] = style_resource_base_url(css_rule_or_declaration);
    if (request->mode() == Fetch::Infrastructure::Request::Mode::CORS && sheet)
        request->set_referrer(base_url);

    // 7. If sheet’s origin-clean flag is set, set req’s initiator type to "css". [CSSOM]
    if (sheet) {
        if (sheet->is_origin_clean())
            request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::CSS);
    } else {
        // AD-HOC: If the resource is not associated with a stylesheet, we must still set an initiator type in order
        //         for this resource to be observable through a PerformanceObserver. WPT relies on this.
        request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Script);
    }

    // 9. Fetch req, with processresponseconsumebody set to processResponse.
    // NB: Implemented by caller.
    return request;
}

// https://drafts.csswg.org/css-values-4/#fetch-a-style-resource
GC::Ptr<Fetch::Infrastructure::FetchController> fetch_a_style_resource(StyleResourceURL const& url_value, RuleOrDeclaration css_rule_or_declaration, Fetch::Infrastructure::Request::Destination destination, CorsMode cors_mode, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction process_response)
{
    auto request = fetch_a_style_resource_impl(url_value, css_rule_or_declaration, destination, cors_mode);
    if (!request)
        return {};

    auto& environment_settings = *css_rule_or_declaration.environment_settings_object;
    auto& vm = environment_settings.vm();

    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_consume_body = move(process_response);

    return Fetch::Fetching::fetch(environment_settings.realm(), *request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));
}

// https://drafts.csswg.org/css-images-4/#fetch-an-external-image-for-a-stylesheet
GC::Ptr<HTML::SharedResourceRequest> fetch_an_external_image_for_a_stylesheet(StyleResourceURL const& url_value, RuleOrDeclaration declaration, DOM::Document& document)
{
    // To fetch an external image for a stylesheet, given a <url> url and a CSS declaration block declaration, fetch a
    // style resource given url, with ruleOrDeclaration being declaration, destination "image", CORS mode "no-cors",
    // and processResponse being the following steps given response res and null, failure or a byte stream byteStream:
    // If byteStream is a byte stream, load the image from the byte stream.

    // NB: We can't directly call fetch_a_style_resource() because we want to make use of SharedResourceRequest to
    //     deduplicate image requests.

    auto request = fetch_a_style_resource_impl(url_value, declaration, Fetch::Infrastructure::Request::Destination::Image, CorsMode::NoCors);
    if (!request)
        return {};

    auto& realm = document.realm();

    auto shared_resource_request = HTML::SharedResourceRequest::get_or_create(realm, document.page(), request->url());
    shared_resource_request->add_callbacks(
        [&document, weak_document = GC::Weak { document }] {
            if (!weak_document)
                return;

            if (auto navigable = document.navigable()) {
                // Once the image has loaded, we need to re-resolve CSS properties that depend on the image's dimensions.
                document.set_needs_paint_only_properties_update();

                // FIXME: Do less than a full repaint if possible?
                document.set_needs_display();
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
