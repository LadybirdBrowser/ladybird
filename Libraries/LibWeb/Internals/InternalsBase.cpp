/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/InternalsBase.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(InternalsBase);

InternalsBase::InternalsBase(JS::Realm& realm)
    : Bindings::Wrappable(realm)
{
}

InternalsBase::~InternalsBase() = default;

HTML::Window& InternalsBase::window() const
{
    return HTML::relevant_window(*this);
}

Page& InternalsBase::page() const
{
    return window().page();
}

}
