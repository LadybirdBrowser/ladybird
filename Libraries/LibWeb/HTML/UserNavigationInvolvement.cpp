/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#event-uni
UserNavigationInvolvement user_navigation_involvement(DOM::Event const& event)
{
    // For convenience at certain call sites, the user navigation involvement for an Event event is defined as follows:

    // 1. Assert: this algorithm is being called as part of an activation behavior definition.
    // 2. Assert: event's type is "click".
    VERIFY(event.type() == "click"_fly_string);

    // 3. If event's isTrusted is initialized to true, then return "activation".
    // 4. Return "none".
    return event.is_trusted() ? UserNavigationInvolvement::Activation : UserNavigationInvolvement::None;
}

}
