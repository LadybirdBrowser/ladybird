/*
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#create-a-potential-cors-request
JS::NonnullGCPtr<Fetch::Infrastructure::Request>
create_potential_CORS_request(JS::VM& vm, URL::URL const& url, Optional<Fetch::Infrastructure::Request::Destination> destination, CORSSettingAttribute cors_attribute_state, SameOriginFallbackFlag same_origin_fallback_flag)
{
    // 1. Let mode be "no-cors" if corsAttributeState is No CORS, and "cors" otherwise.
    auto mode = cors_attribute_state == CORSSettingAttribute::NoCORS
        ? Fetch::Infrastructure::Request::Mode::NoCORS
        : Fetch::Infrastructure::Request::Mode::CORS;

    // 2. If same-origin fallback flag is set and mode is "no-cors", set mode to "same-origin".
    if (same_origin_fallback_flag == SameOriginFallbackFlag::Yes && mode == Fetch::Infrastructure::Request::Mode::NoCORS)
        mode = Fetch::Infrastructure::Request::Mode::SameOrigin;

    // 3. Let credentialsMode be "include".
    auto credentials_mode = Fetch::Infrastructure::Request::CredentialsMode::Include;

    // 4. If corsAttributeState is Anonymous, set credentialsMode to "same-origin".
    if (cors_attribute_state == CORSSettingAttribute::Anonymous)
        credentials_mode = Fetch::Infrastructure::Request::CredentialsMode::SameOrigin;

    // 5. Let request be a new request whose URL is url, destination is destination, mode is mode, credentials mode is credentialsMode,
    // and whose use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(url);
    request->set_destination(destination);
    request->set_mode(mode);
    request->set_credentials_mode(credentials_mode);
    request->set_use_url_credentials(true);

    return request;
}

}
