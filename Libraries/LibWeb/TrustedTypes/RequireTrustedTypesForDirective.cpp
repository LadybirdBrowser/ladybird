/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>

#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(RequireTrustedTypesForDirective);

RequireTrustedTypesForDirective::RequireTrustedTypesForDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://www.w3.org/TR/trusted-types/#require-trusted-types-for-pre-navigation-check
ContentSecurityPolicy::Directives::Directive::Result RequireTrustedTypesForDirective::pre_navigation_check(GC::Ref<Fetch::Infrastructure::Request> request, NavigationType, GC::Ref<ContentSecurityPolicy::Policy const>) const
{
    // 1. If request’s url’s scheme is not "javascript", return "Allowed" and abort further steps.
    if (request->url().scheme() != "javascript"sv)
        return Result::Allowed;

    // 2. Let urlString be the result of running the URL serializer on request’s url.
    auto url_string = request->url().serialize();

    // 3. Let encodedScriptSource be the result of removing the leading "javascript:" from urlString.
    auto const encoded_script_source = MUST(url_string.substring_from_byte_offset("javascript:"sv.length()));

    // 4. Let convertedScriptSource be the result of executing Process value with a default policy algorithm, with the following arguments:
    //    expectedType:
    //      TrustedScript
    //    global:
    //      request’s clients’s global object:
    //    input:
    //      encodedScriptSource:
    //    sink:
    //      "Location href":
    auto converted_script_source = process_value_with_a_default_policy(
        TrustedTypeName::TrustedScript,
        request->client()->global_object(),
        Utf16String::from_utf8(encoded_script_source),
        InjectionSink::Locationhref);

    // If that algorithm threw an error or convertedScriptSource is not a TrustedScript object, return "Blocked" and abort further steps.
    if (converted_script_source.is_error() || !converted_script_source.value().has_value())
        return Result::Blocked;

    auto const* converted_script_source_value = converted_script_source.value().value().get_pointer<GC::Root<TrustedScript>>();

    if (!converted_script_source_value)
        return Result::Blocked;

    // 5. Set urlString to be the result of prepending "javascript:" to stringified convertedScriptSource.
    url_string = MUST(String::formatted("javascript:{}", (*converted_script_source_value)->to_string()));

    // 6. Let newURL be the result of running the URL parser on urlString. If the parser returns a failure, return "Blocked" and abort further steps.
    auto const new_url = DOMURL::parse(url_string);
    if (!new_url.has_value())
        return Result::Blocked;

    // 7. Set request’s url to newURL.
    request->set_url(new_url.value());

    // 8. Return "Allowed".
    return Result::Allowed;
}

}
