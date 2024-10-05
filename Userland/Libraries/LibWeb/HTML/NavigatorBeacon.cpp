/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/NavigatorBeacon.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

// https://w3c.github.io/beacon/#sendbeacon-method
WebIDL::ExceptionOr<bool> NavigatorBeaconMixin::send_beacon(String const& url, Optional<Fetch::BodyInit> const& data)
{
    auto& navigator = verify_cast<Navigator>(*this);
    auto& realm = navigator.realm();
    auto& vm = realm.vm();
    auto& relevant_settings_object = HTML::relevant_settings_object(navigator);

    // 1. Set base to this's relevant settings object's API base URL.
    auto base_url = relevant_settings_object.api_base_url();

    // 2. Set origin to this's relevant settings object's origin.
    auto origin = relevant_settings_object.origin();

    // 3. Set parsedUrl to the result of the URL parser steps with url and base. If the algorithm returns an error, or if parsedUrl's scheme is not "http" or "https", throw a "TypeError" exception and terminate these steps.
    auto parsed_url = URL::Parser::basic_parse(url, base_url);
    if (!parsed_url.is_valid())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Beacon URL {} is invalid.", url)) };
    if (parsed_url.scheme() != "http" && parsed_url.scheme() != "https")
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Beacon URL {} must be either http:// or https://.", url)) };

    // 4. Let headerList be an empty list.
    auto header_list = Fetch::Infrastructure::HeaderList::create(vm);

    // 5. Let corsMode be "no-cors".
    auto cors_mode = Fetch::Infrastructure::Request::Mode::NoCORS;

    // 6. If data is not null:
    JS::GCPtr<Fetch::Infrastructure::Body> transmitted_data;
    if (data.has_value()) {
        // 6.1 Set transmittedData and contentType to the result of extracting data's byte stream with the keepalive flag set.
        auto body_with_type = TRY(Fetch::extract_body(realm, data.value(), true));
        transmitted_data = body_with_type.body;
        auto& content_type = body_with_type.type;

        // 6.2 If the amount of data that can be queued to be sent by keepalive enabled requests is exceeded by the size of transmittedData (as defined in HTTP-network-or-cache fetch), set the return value to false and terminate these steps.
        if (transmitted_data->length().has_value() && transmitted_data->length().value() > Fetch::Fetching::keepalive_maximum_size)
            return false;

        // 6.3 If contentType is not null:
        if (content_type.has_value()) {
            // Set corsMode to "cors".
            cors_mode = Fetch::Infrastructure::Request::Mode::CORS;

            // If contentType value is a CORS-safelisted request-header value for the Content-Type header, set corsMode to "no-cors".
            auto content_type_header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, content_type.value());
            if (Fetch::Infrastructure::is_cors_safelisted_request_header(content_type_header))
                cors_mode = Fetch::Infrastructure::Request::Mode::NoCORS;

            // Append a Content-Type header with value contentType to headerList.
            header_list->append(content_type_header);
        }
    }

    // FIXME: 7. Set the return value to true, return the sendBeacon() call, and continue to run the following steps in parallel:

    // 7.1 Let req be a new request, initialized as follows:
    auto req = Fetch::Infrastructure::Request::create(vm);
    req->set_method(MUST(ByteBuffer::copy("POST"sv.bytes()))); // method: POST
    req->set_client(&relevant_settings_object);                // client: this's relevant settings object
    req->set_url_list({ parsed_url });                         // url: parsedUrl
    req->set_header_list(header_list);                         // header list: headerList
    req->set_origin(origin);                                   // origin: origin
    req->set_keepalive(true);                                  // keepalive: true
    if (transmitted_data)
        req->set_body(JS::NonnullGCPtr<Fetch::Infrastructure::Body> { *transmitted_data }); // body: transmittedData
    req->set_mode(cors_mode);                                                               // mode: corsMode
    req->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);    // credentials mode: include
    req->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Beacon);         // initiator type: "beacon"

    // 7.2 Fetch req.
    (void)Fetch::Fetching::fetch(realm, req, Fetch::Infrastructure::FetchAlgorithms::create(vm, {}));

    return true;
}

}
