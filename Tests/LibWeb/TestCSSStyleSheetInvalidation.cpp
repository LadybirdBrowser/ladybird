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

    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":host > *"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":host + .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host(":host ~ .item"sv));
    EXPECT(CSS::selector_may_match_light_dom_under_shadow_host("::slotted(.item)"sv));
}

}
