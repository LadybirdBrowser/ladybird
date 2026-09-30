/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::MixedContent {

// https://w3c.github.io/webappsec-mixed-content/#categorize-settings-object
enum class ProhibitsMixedSecurityContexts : u8 {
    ProhibitsMixedSecurityContexts,
    DoesNotRestrictMixedSecurityContexts,
};

}
