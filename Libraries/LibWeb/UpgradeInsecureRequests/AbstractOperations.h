/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/UpgradeInsecureRequests/InsecureRequestsPolicy.h>

namespace Web::UpgradeInsecureRequests {

void inherit_the_insecure_requests_policy_into_a_nested_browsing_context(HTML::BrowsingContext&, InsecureRequestsPolicy embedding_document_policy, UpgradeInsecureNavigationsSet const& embedding_document_set);
void inherit_the_insecure_requests_policy_into_a_new_document(HTML::EnvironmentSettingsObject& document_settings, HTML::BrowsingContext const&);
void inherit_the_insecure_requests_policy_into_a_worker(HTML::EnvironmentSettingsObject& settings_object, InsecureRequestsPolicy outside_settings_policy, UpgradeInsecureNavigationsSet const& outside_settings_set);

WEB_API void upgrade_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request&);

InsecureRequestsPolicy should_insecure_requests_be_upgraded_for_client(GC::Ptr<HTML::EnvironmentSettingsObject>);

}
