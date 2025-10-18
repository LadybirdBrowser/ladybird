/*
 * Copyright (c) 2023, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#user-navigation-involvement
enum class UserNavigationInvolvement {
    BrowserUI,
    Activation,
    None,
};

UserNavigationInvolvement user_navigation_involvement(DOM::Event const&);

}
