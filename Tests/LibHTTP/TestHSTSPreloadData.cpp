/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HSTSPreloadData.h>
#include <LibTest/TestCase.h>

// Hosts from Chromium's transport_security_state_static.json:
//  - accounts.google.com: a preloaded force-https host (exact match).
//  - dev: a force-https TLD with include_subdomains, so any *.dev matches as a subdomain.
//  - example.test: a reserved TLD that is never preloaded.
// Stable enough to anchor tests; if upstream removes one the matching test fails loudly
// rather than silently regressing.
//
// is_known_preloaded_hsts_host expects an already-lowercased domain (callers canonicalize
// before querying), so these tests pass lowercased input.

TEST_CASE(congruent_match)
{
    EXPECT(HTTP::HSTSPreloadData::the().is_known_preloaded_hsts_host("accounts.google.com"sv));
}

TEST_CASE(subdomain_match_via_include_subdomains)
{
    EXPECT(HTTP::HSTSPreloadData::the().is_known_preloaded_hsts_host("anything.dev"sv));
}

TEST_CASE(non_preloaded_host_is_not_known)
{
    EXPECT(!HTTP::HSTSPreloadData::the().is_known_preloaded_hsts_host("example.test"sv));
}
