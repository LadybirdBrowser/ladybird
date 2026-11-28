/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWebCommon/MixedContent/ProhibitsMixedSecurityContexts.h>

namespace Web::MixedContent {

ProhibitsMixedSecurityContexts does_settings_prohibit_mixed_security_contexts(HTML::EnvironmentSettingsObject&);

}
