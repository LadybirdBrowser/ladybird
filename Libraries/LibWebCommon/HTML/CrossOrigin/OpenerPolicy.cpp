/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibWebCommon/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-coop
// NB: response is given as its header list, and reservedEnvironment as whether it is a non-secure context, which is all
//     these steps read of them.
OpenerPolicy obtain_an_opener_policy(HTTP::HeaderList const& header_list, bool reserved_environment_is_a_non_secure_context)
{
    // 1. Let policy be a new opener policy.
    OpenerPolicy policy;

    // 2. If reservedEnvironment is a non-secure context, then return policy.
    if (reserved_environment_is_a_non_secure_context)
        return policy;

    // 3. Let parsedItem be the result of getting a structured field value given `Cross-Origin-Opener-Policy` and "item"
    //    from response's header list.
    auto parsed_item = header_list.get_structured_field_value<HTTP::StructuredFields::Item>("Cross-Origin-Opener-Policy"sv);

    // 4. If parsedItem is not null and parsedItem[0] is a token:
    if (parsed_item.has_value() && parsed_item->bare_item.has<HTTP::StructuredFields::Token>()) {
        auto const& token = parsed_item->bare_item.get<HTTP::StructuredFields::Token>().value;

        // 1. If parsedItem[0] is "same-origin":
        if (token == "same-origin"sv) {
            // 1. Let coep be the result of obtaining a cross-origin embedder policy from response and
            //    reservedEnvironment.
            auto coep = obtain_an_embedder_policy(header_list, reserved_environment_is_a_non_secure_context);

            // 2. If coep's value is compatible with cross-origin isolation, then set policy's value to
            //    "same-origin-plus-COEP".
            if (is_compatible_with_cross_origin_isolation(coep.value)) {
                policy.value = OpenerPolicyValue::SameOriginPlusCOEP;
            }
            // 3. Otherwise, set policy's value to "same-origin".
            else {
                policy.value = OpenerPolicyValue::SameOrigin;
            }
        }
        // 2. If parsedItem[0] is "same-origin-allow-popups", then set policy's value to "same-origin-allow-popups".
        else if (token == "same-origin-allow-popups"sv) {
            policy.value = OpenerPolicyValue::SameOriginAllowPopups;
        }
        // 3. If parsedItem[0] is "noopener-allow-popups", then set policy's value to "noopener-allow-popups".
        else if (token == "noopener-allow-popups"sv) {
            policy.value = OpenerPolicyValue::NoopenerAllowPopups;
        }

        // 4. If parsedItem[1]["report-to"] exists and it is a string, then set policy's reporting endpoint to
        //    parsedItem[1]["report-to"].
        if (auto report_to = parsed_item->parameters.get("report-to"sv); report_to.has_value() && report_to->has<ByteString>())
            policy.reporting_endpoint = Utf16String::from_utf8(report_to->get<ByteString>());
    }

    // 5. Set parsedItem to the result of getting a structured field value given `Cross-Origin-Opener-Policy-Report-Only`
    //    and "item" from response's header list.
    parsed_item = header_list.get_structured_field_value<HTTP::StructuredFields::Item>("Cross-Origin-Opener-Policy-Report-Only"sv);

    // 6. If parsedItem is not null and parsedItem[0] is a token:
    if (parsed_item.has_value() && parsed_item->bare_item.has<HTTP::StructuredFields::Token>()) {
        auto const& token = parsed_item->bare_item.get<HTTP::StructuredFields::Token>().value;

        // 1. If parsedItem[0] is "same-origin":
        if (token == "same-origin"sv) {
            // 1. Let coep be the result of obtaining a cross-origin embedder policy from response and
            //    reservedEnvironment.
            auto coep = obtain_an_embedder_policy(header_list, reserved_environment_is_a_non_secure_context);

            // 2. If coep's value is compatible with cross-origin isolation or coep's report-only value is compatible
            //    with cross-origin isolation, then set policy's report-only value to "same-origin-plus-COEP".
            if (is_compatible_with_cross_origin_isolation(coep.value) || is_compatible_with_cross_origin_isolation(coep.report_only_value)) {
                policy.report_only_value = OpenerPolicyValue::SameOriginPlusCOEP;
            }
            // 3. Otherwise, set policy's report-only value to "same-origin".
            else {
                policy.report_only_value = OpenerPolicyValue::SameOrigin;
            }
        }
        // 2. If parsedItem[0] is "same-origin-allow-popups", then set policy's report-only value to
        //    "same-origin-allow-popups".
        else if (token == "same-origin-allow-popups"sv) {
            policy.report_only_value = OpenerPolicyValue::SameOriginAllowPopups;
        }

        // 3. If parsedItem[1]["report-to"] exists and it is a string, then set policy's report-only reporting endpoint
        //    to parsedItem[1]["report-to"].
        if (auto report_to = parsed_item->parameters.get("report-to"sv); report_to.has_value() && report_to->has<ByteString>())
            policy.report_only_reporting_endpoint = Utf16String::from_utf8(report_to->get<ByteString>());
    }

    // 7. Return policy.
    return policy;
}

}
