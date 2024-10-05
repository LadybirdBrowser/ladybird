/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/SecureContexts/AbstractOperations.h>

namespace Web::SecureContexts {

// https://w3c.github.io/webappsec-secure-contexts/#is-origin-trustworthy
Trustworthiness is_origin_potentially_trustworthy(URL::Origin const& origin)
{
    // 1. If origin is an opaque origin, return "Not Trustworthy".
    if (origin.is_opaque())
        return Trustworthiness::NotTrustworthy;

    // 2. Assert: origin is a tuple origin.

    // 3. If origin’s scheme is either "https" or "wss", return "Potentially Trustworthy".
    // Note: This is meant to be analog to the a priori authenticated URL concept in [MIX].
    if (origin.scheme().is_one_of("https"sv, "wss"sv))
        return Trustworthiness::PotentiallyTrustworthy;

    // 4. If origin’s host matches one of the CIDR notations 127.0.0.0/8 or ::1/128 [RFC4632], return "Potentially Trustworthy".
    // FIXME: This would be nicer if URL::IPv4Address and URL::IPv6Address were instances of AK::IPv4Address and AK::IPv6Address
    if (origin.host().has<URL::IPv4Address>()) {
        if ((origin.host().get<URL::IPv4Address>() & 0xff000000) != 0)
            return Trustworthiness::PotentiallyTrustworthy;
    } else if (origin.host().has<URL::IPv6Address>()) {
        auto ipv6_address = origin.host().get<URL::IPv6Address>();
        static constexpr URL::IPv6Address loopback { 0, 0, 0, 0, 0, 0, 0, 1 };
        if (ipv6_address == loopback)
            return Trustworthiness::PotentiallyTrustworthy;
    }

    // 5. If the user agent conforms to the name resolution rules in [let-localhost-be-localhost] and one of the following is true:
    // - origin’s host is "localhost" or "localhost."
    // - origin’s host ends with ".localhost" or ".localhost."
    // then return "Potentially Trustworthy".
    // Note: See § 5.2 localhost for details on the requirements here.
    if (origin.host().has<String>()) {
        auto const& host = origin.host().get<String>();
        if (host.is_one_of("localhost"sv, "localhost.")
            || host.ends_with_bytes(".localhost"sv)
            || host.ends_with_bytes(".localhost."sv)) {
            return Trustworthiness::PotentiallyTrustworthy;
        }
    }

    // 6. If origin’s scheme is "file", return "Potentially Trustworthy".
    // AD-HOC: Our resource:// is basically an alias to file://
    if (origin.scheme() == "file"sv || origin.scheme() == "resource"sv)
        return Trustworthiness::PotentiallyTrustworthy;

    // 7. If origin’s scheme component is one which the user agent considers to be authenticated, return "Potentially Trustworthy".
    // Note: See § 7.1 Packaged Applications for detail here.

    // 8. If origin has been configured as a trustworthy origin, return "Potentially Trustworthy".
    // Note: See § 7.2 Development Environments for detail here.

    // 9. Return "Not Trustworthy".
    return Trustworthiness::NotTrustworthy;
}

// https://w3c.github.io/webappsec-secure-contexts/#is-url-trustworthy
Trustworthiness is_url_potentially_trustworthy(URL::URL const& url)
{
    // 1. If url is "about:blank" or "about:srcdoc", return "Potentially Trustworthy".
    if (url == "about:blank"sv || url == "about:srcdoc"sv)
        return Trustworthiness::PotentiallyTrustworthy;

    // 2. If url’s scheme is "data", return "Potentially Trustworthy".
    if (url.scheme() == "data"sv)
        return Trustworthiness::PotentiallyTrustworthy;

    // 3. Return the result of executing § 3.1 Is origin potentially trustworthy? on url’s origin.
    return is_origin_potentially_trustworthy(url.origin());
}

}
