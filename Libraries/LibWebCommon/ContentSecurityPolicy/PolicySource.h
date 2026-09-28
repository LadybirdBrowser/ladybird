/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::ContentSecurityPolicy {

// https://w3c.github.io/webappsec-csp/#policy-source
// Each policy has a source, which is either "header" or "meta".
enum class PolicySource {
    Header,
    Meta,
};

}
