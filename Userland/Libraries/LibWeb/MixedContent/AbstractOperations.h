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

enum class ProhibitsMixedSecurityContexts {
    ProhibitsMixedSecurityContexts,
    DoesNotRestrictMixedSecurityContexts,
};

ProhibitsMixedSecurityContexts does_settings_prohibit_mixed_security_contexts(JS::GCPtr<HTML::EnvironmentSettingsObject>);

Fetch::Infrastructure::RequestOrResponseBlocking should_fetching_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request&);

}
