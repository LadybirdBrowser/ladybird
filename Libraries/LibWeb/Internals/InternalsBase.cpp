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
    : Bindings::PlatformObject(realm)
{
}

InternalsBase::~InternalsBase() = default;

HTML::Window& InternalsBase::window() const
{
    return as<HTML::Window>(HTML::relevant_global_object(*this));
}

Page& InternalsBase::page() const
{
    return window().page();
}

}
