/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/HTML/CrossOrigin/OpenerPolicyEnforcement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/origin.html#matching-coop
bool match_opener_policy_values(OpenerPolicyValue document_coop, URL::Origin const& document_origin, OpenerPolicyValue response_coop, URL::Origin const& response_origin)
{
    // 1. If documentCOOP is "unsafe-none" and responseCOOP is "unsafe-none", then return true.
    if (document_coop == OpenerPolicyValue::UnsafeNone && response_coop == OpenerPolicyValue::UnsafeNone)
        return true;

    // 2. If documentCOOP is "unsafe-none" or responseCOOP is "unsafe-none", then return false.
    if (document_coop == OpenerPolicyValue::UnsafeNone || response_coop == OpenerPolicyValue::UnsafeNone)
        return false;

    // 3. If documentCOOP is responseCOOP and documentOrigin is same origin with responseOrigin, then return true.
    if (document_coop == response_coop && document_origin.is_same_origin(response_origin))
        return true;

    // 4. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/origin.html#check-browsing-context-group-switch-coop-value-popup
bool check_if_popup_coop_values_require_a_browsing_context_group_switch(URL::Origin const& response_origin, URL::Origin const& active_document_navigation_origin, OpenerPolicyValue response_coop_value, OpenerPolicyValue active_document_coop_value)
{
    // 1. If responseCOOPValue is "noopener-allow-popups", then return true.
    if (response_coop_value == OpenerPolicyValue::NoopenerAllowPopups)
        return true;

    // 2. If all of the following are true:
    //    - activeDocumentCOOPValue is "same-origin-allow-popups" or "noopener-allow-popups"; and
    //    - responseCOOPValue is "unsafe-none",
    //    then return false.
    if ((active_document_coop_value == OpenerPolicyValue::SameOriginAllowPopups || active_document_coop_value == OpenerPolicyValue::NoopenerAllowPopups)
        && response_coop_value == OpenerPolicyValue::UnsafeNone) {
        return false;
    }

    // 3. If the result of matching activeDocumentCOOPValue, activeDocumentNavigationOrigin, responseCOOPValue, and
    //    responseOrigin is true, then return false.
    if (match_opener_policy_values(active_document_coop_value, active_document_navigation_origin, response_coop_value, response_origin))
        return false;

    // 4. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/origin.html#check-browsing-context-group-switch-coop-value
bool check_if_coop_values_require_a_browsing_context_group_switch(bool is_initial_about_blank, URL::Origin const& response_origin, URL::Origin const& active_document_navigation_origin, OpenerPolicyValue response_coop_value, OpenerPolicyValue active_document_coop_value)
{
    // 1. If isInitialAboutBlank is true, then return the result of checking if popup COOP values requires a browsing
    //    context group switch with responseOrigin, activeDocumentNavigationOrigin, responseCOOPValue, and
    //    activeDocumentCOOPValue.
    if (is_initial_about_blank)
        return check_if_popup_coop_values_require_a_browsing_context_group_switch(response_origin, active_document_navigation_origin, response_coop_value, active_document_coop_value);

    // 2. If the result of matching activeDocumentCOOPValue, activeDocumentNavigationOrigin, responseCOOPValue, and
    //    responseOrigin is true, then return false.
    if (match_opener_policy_values(active_document_coop_value, active_document_navigation_origin, response_coop_value, response_origin))
        return false;

    // 3. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/origin.html#check-bcg-switch-navigation-report-only
bool check_if_enforcing_report_only_coop_would_require_a_browsing_context_group_switch(bool is_initial_about_blank, URL::Origin const& response_origin, URL::Origin const& active_document_navigation_origin, OpenerPolicy const& response_coop, OpenerPolicy const& active_document_coop)
{
    // 1. If the result of checking if COOP values require a browsing context group switch given isInitialAboutBlank,
    //    responseOrigin, activeDocumentNavigationOrigin, responseCOOP's report-only value, and activeDocumentCOOP's
    //    report-only value is false, then return false.
    if (!check_if_coop_values_require_a_browsing_context_group_switch(is_initial_about_blank, response_origin, active_document_navigation_origin, response_coop.report_only_value, active_document_coop.report_only_value))
        return false;

    // 2. If the result of checking if COOP values require a browsing context group switch given isInitialAboutBlank,
    //    responseOrigin, activeDocumentNavigationOrigin, responseCOOP's value, and activeDocumentCOOP's report-only
    //    value is true, then return true.
    if (check_if_coop_values_require_a_browsing_context_group_switch(is_initial_about_blank, response_origin, active_document_navigation_origin, response_coop.value, active_document_coop.report_only_value))
        return true;

    // 3. If the result of checking if COOP values require a browsing context group switch given isInitialAboutBlank,
    //    responseOrigin, activeDocumentNavigationOrigin, responseCOOP's report-only value, and activeDocumentCOOP's
    //    value is true, then return true.
    if (check_if_coop_values_require_a_browsing_context_group_switch(is_initial_about_blank, response_origin, active_document_navigation_origin, response_coop.report_only_value, active_document_coop.value))
        return true;

    // 4. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/origin.html#coop-enforce
// NB: browsingContext is given as whether its active document is initial about:blank, which is all these steps read of it.
OpenerPolicyEnforcementResult enforce_a_responses_opener_policy(bool is_initial_about_blank, URL::URL const& response_url, URL::Origin const& response_origin, OpenerPolicy const& response_coop, OpenerPolicyEnforcementResult const& current_coop_enforcement_result)
{
    // 1. Let newCOOPEnforcementResult be a new opener policy enforcement result with
    //    - needs a browsing context group switch: currentCOOPEnforcementResult's needs a browsing context group switch
    //    - would need a browsing context group switch due to report-only: currentCOOPEnforcementResult's would need a
    //      browsing context group switch due to report-only
    //    - url: responseURL
    //    - origin: responseOrigin
    //    - opener policy: responseCOOP
    //    - current context is navigation source: true
    OpenerPolicyEnforcementResult new_coop_enforcement_result {
        .needs_a_browsing_context_group_switch = current_coop_enforcement_result.needs_a_browsing_context_group_switch,
        .would_need_a_browsing_context_group_switch_due_to_report_only = current_coop_enforcement_result.would_need_a_browsing_context_group_switch_due_to_report_only,
        .url = response_url,
        .origin = response_origin,
        .opener_policy = response_coop,
        .current_context_is_navigation_source = true,
    };

    // 2. Let isInitialAboutBlank be browsingContext's active document's is initial about:blank.
    // FIXME: 3. If isInitialAboutBlank is true and browsingContext's initial URL is null, set browsingContext's initial URL
    //           to responseURL.

    // 4. If the result of checking if COOP values require a browsing context group switch given isInitialAboutBlank,
    //    responseOrigin, currentCOOPEnforcementResult's origin, responseCOOP's value, and
    //    currentCOOPEnforcementResult's opener policy's value is true:
    if (check_if_coop_values_require_a_browsing_context_group_switch(is_initial_about_blank, response_origin, current_coop_enforcement_result.origin, response_coop.value, current_coop_enforcement_result.opener_policy.value)) {
        // 1. Set newCOOPEnforcementResult's needs a browsing context group switch to true.
        new_coop_enforcement_result.needs_a_browsing_context_group_switch = true;

        // FIXME: 2. If browsingContext's group's browsing context set's size is greater than 1:
        //           1. Queue a violation report for browsing context group switch when navigating to a COOP response
        //              with responseCOOP, "enforce", responseURL, currentCOOPEnforcementResult's url,
        //              currentCOOPEnforcementResult's origin, responseOrigin, and referrer.
        //           2. Queue a violation report for browsing context group switch when navigating away from a COOP
        //              response with currentCOOPEnforcementResult's opener policy, "enforce",
        //              currentCOOPEnforcementResult's url, responseURL, currentCOOPEnforcementResult's origin,
        //              responseOrigin, and currentCOOPEnforcementResult's current context is navigation source.
    }

    // 5. If the result of checking if enforcing report-only COOP would require a browsing context group switch given
    //    isInitialAboutBlank, responseOrigin, currentCOOPEnforcementResult's origin, responseCOOP, and
    //    currentCOOPEnforcementResult's opener policy is true:
    if (check_if_enforcing_report_only_coop_would_require_a_browsing_context_group_switch(is_initial_about_blank, response_origin, current_coop_enforcement_result.origin, response_coop, current_coop_enforcement_result.opener_policy)) {
        // 1. Set newCOOPEnforcementResult's would need a browsing context group switch due to report-only to true.
        new_coop_enforcement_result.would_need_a_browsing_context_group_switch_due_to_report_only = true;

        // FIXME: 2. If browsingContext's group's browsing context set's size is greater than 1:
        //           1. Queue a violation report for browsing context group switch when navigating to a COOP response
        //              with responseCOOP, "reporting", responseURL, currentCOOPEnforcementResult's url,
        //              currentCOOPEnforcementResult's origin, responseOrigin, and referrer.
        //           2. Queue a violation report for browsing context group switch when navigating away from a COOP
        //              response with currentCOOPEnforcementResult's opener policy, "reporting",
        //              currentCOOPEnforcementResult's url, responseURL, currentCOOPEnforcementResult's origin,
        //              responseOrigin, and currentCOOPEnforcementResult's current context is navigation source.
    }

    // 6. Return newCOOPEnforcementResult.
    return new_coop_enforcement_result;
}

}
