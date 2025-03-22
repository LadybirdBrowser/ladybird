/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ProcessesPrototype.h>
#include <LibWeb/Internals/Processes.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(Processes);

Processes::Processes(JS::Realm& realm)
    : InternalsBase(realm)
{
}

Processes::~Processes() = default;

void Processes::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Processes);
}

void Processes::update_process_statistics()
{
    page().client().update_process_statistics();
}

}
