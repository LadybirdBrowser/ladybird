/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Fetch/Infrastructure/RequestOrResponseBlocking.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::MixedContent {

void upgrade_a_mixed_content_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request&);

enum class ProhibitsMixedSecurityContexts {
    ProhibitsMixedSecurityContexts,
    DoesNotRestrictMixedSecurityContexts,
};

ProhibitsMixedSecurityContexts does_settings_prohibit_mixed_security_contexts(JS::GCPtr<HTML::EnvironmentSettingsObject>);

Fetch::Infrastructure::RequestOrResponseBlocking should_fetching_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request&);

Fetch::Infrastructure::RequestOrResponseBlocking should_response_to_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request&, JS::NonnullGCPtr<Fetch::Infrastructure::Response>&);

}
