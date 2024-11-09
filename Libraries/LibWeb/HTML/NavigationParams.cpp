/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(NavigationParams);
JS_DEFINE_ALLOCATOR(NonFetchSchemeNavigationParams);

void NavigationParams::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(navigable);
    visitor.visit(request);
    visitor.visit(response);
    visitor.visit(fetch_controller);
    visitor.visit(reserved_environment);
}

void NonFetchSchemeNavigationParams::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(navigable);
}

}
