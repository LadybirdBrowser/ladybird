/*
 * Copyright (c) 2023, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

UserNavigationInvolvement user_navigation_involvement(DOM::Event const&);

}
