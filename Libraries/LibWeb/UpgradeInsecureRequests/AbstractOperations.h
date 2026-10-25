/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::UpgradeInsecureRequests {

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#insecure-requests-policy
enum class InsecureRequestsPolicy {
    DoNotUpgrade,
    Upgrade,
};

WEB_API void upgrade_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request&);

InsecureRequestsPolicy should_insecure_requests_be_upgraded_for_client(GC::Ptr<HTML::EnvironmentSettingsObject>);

}
