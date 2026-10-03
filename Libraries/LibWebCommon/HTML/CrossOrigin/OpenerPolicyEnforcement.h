/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWebCommon/HTML/CrossOrigin/OpenerPolicyEnforcementResult.h>

namespace Web::HTML {

WEBCOMMON_API bool match_opener_policy_values(OpenerPolicyValue document_coop, URL::Origin const& document_origin, OpenerPolicyValue response_coop, URL::Origin const& response_origin);
WEBCOMMON_API bool check_if_popup_coop_values_require_a_browsing_context_group_switch(URL::Origin const& response_origin, URL::Origin const& active_document_navigation_origin, OpenerPolicyValue response_coop_value, OpenerPolicyValue active_document_coop_value);
WEBCOMMON_API bool check_if_coop_values_require_a_browsing_context_group_switch(bool is_initial_about_blank, URL::Origin const& response_origin, URL::Origin const& active_document_navigation_origin, OpenerPolicyValue response_coop_value, OpenerPolicyValue active_document_coop_value);
WEBCOMMON_API bool check_if_enforcing_report_only_coop_would_require_a_browsing_context_group_switch(bool is_initial_about_blank, URL::Origin const& response_origin, URL::Origin const& active_document_navigation_origin, OpenerPolicy const& response_coop, OpenerPolicy const& active_document_coop);
WEBCOMMON_API OpenerPolicyEnforcementResult enforce_a_responses_opener_policy(bool is_initial_about_blank, URL::URL const& response_url, URL::Origin const& response_origin, OpenerPolicy const& response_coop, OpenerPolicyEnforcementResult const& current_coop_enforcement_result);

}
