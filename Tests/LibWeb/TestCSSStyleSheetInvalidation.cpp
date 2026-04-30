/*
 * Copyright (c) 2026-present, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>

namespace Web {

TEST_CASE(selector_escape_to_light_dom_under_shadow_host)
{
    EXPECT(!CSS::selector_may_match_light_dom_under_shadow_host(":host"sv));
    EXPECT(!CSS::selector_may_match_light_dom_under_shadow_host(":host(.active)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_under_shadow_host(":is(:host)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_under_shadow_host(":where(:host)"sv));

    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":host > *"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":host + .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":host ~ .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":is(:host) > *"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":where(:host) .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":is(.foo, :host) + .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host("::slotted(.item)"sv));
}

TEST_CASE(selector_escape_to_light_dom_outside_shadow_host)
{
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":host"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":host(.active)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":host > *"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":host *"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host("::slotted(.item)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":is(:host)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":where(:host)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":is(:host) > *"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":where(:host) *"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":is(:host > .item)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":where(:host .item)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":host:has(+ .item)"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":has(:host) + .item"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":not(:host) + .item"sv));
    EXPECT(!CSS::selector_may_match_light_dom_outside_shadow_host(":nth-child(1 of :host) + .item"sv));

    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":host + .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":host ~ .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":host + :has(*)"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":is(:host) + .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":where(:host) ~ .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":is(:host) + :has(*)"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":is(.foo, :host) + .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":where(.foo, :host(.active)) + .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":is(:where(:host)) + .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":is(:host + .item)"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":where(:host ~ .item)"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":is(:host + .item) > .child"sv));
    EXPECT(CSS::selector_may_match_light_dom_outside_shadow_host(":is(.foo, :where(:host + .item))"sv));
}

}
