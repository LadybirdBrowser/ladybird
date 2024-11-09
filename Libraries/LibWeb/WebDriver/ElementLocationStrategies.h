/*
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-table-of-location-strategies
enum class LocationStrategy {
    CssSelector,
    LinkText,
    PartialLinkText,
    TagName,
    XPath,
};

Optional<LocationStrategy> location_strategy_from_string(StringView type);
ErrorOr<JS::NonnullGCPtr<DOM::NodeList>, Error> invoke_location_strategy(LocationStrategy type, DOM::ParentNode& start_node, StringView selector);

}
