/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-4/#fetch-a-style-resource
void fetch_a_style_resource(String const& url_value, CSSStyleSheet const& sheet, Fetch::Infrastructure::Request::Destination destination, CorsMode cors_mode, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction process_response)
{
    auto& vm = sheet.vm();

    // 1. Let environmentSettings be sheet’s relevant settings object.
    auto& environment_settings = HTML::relevant_settings_object(sheet);

    // 2. Let base be sheet’s stylesheet base URL if it is not null, otherwise environmentSettings’s API base URL. [CSSOM]
    auto base = sheet.base_url().value_or(environment_settings.api_base_url());

    // 3. Let parsedUrl be the result of the URL parser steps with urlValue’s url and base. If the algorithm returns an error, return.
    auto parsed_url = URL::Parser::basic_parse(url_value, base);
    if (!parsed_url.has_value())
        return;

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

    // 5. Apply any URL request modifier steps that apply to this request.
    // FIXME: No specs seem to define these yet. When they do, implement them.

    // 6. If req’s mode is "cors", set req’s referrer to sheet’s location. [CSSOM]
    if (request->mode() == Fetch::Infrastructure::Request::Mode::CORS) {
        // FIXME: sheet's location is an optional string, what do we do here?
    }

    // 7. If sheet’s origin-clean flag is set, set req’s initiator type to "css". [CSSOM]
    if (sheet.is_origin_clean())
        request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::CSS);

    // 8. Fetch req, with processresponseconsumebody set to processResponse.
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_consume_body = move(process_response);

    (void)Fetch::Fetching::fetch(environment_settings.realm(), request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));
}

}
