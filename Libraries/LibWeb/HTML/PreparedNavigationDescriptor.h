/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>

namespace Web::HTML {

WEB_API PreparedNavigationDescriptor create_prepared_navigation_descriptor(PreparedNavigation const&);
WEB_API PreparedNavigation create_prepared_navigation_from_descriptor(JS::Realm&, PreparedNavigationDescriptor);

}
