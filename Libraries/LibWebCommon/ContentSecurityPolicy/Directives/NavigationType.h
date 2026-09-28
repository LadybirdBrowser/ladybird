/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-pre-navigation-check
// The navigation type passed to a directive's pre-navigation and navigation response checks: "form-submission" or
// "other".
enum class NavigationType {
    FormSubmission,
    Other,
};

}
