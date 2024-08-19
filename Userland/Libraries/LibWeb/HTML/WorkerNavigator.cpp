/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WorkerNavigatorPrototype.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerNavigator.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerNavigator);

GC::Ref<WorkerNavigator> WorkerNavigator::create(WorkerGlobalScope& global_scope)
{
    return global_scope.heap().allocate<WorkerNavigator>(global_scope.realm(), global_scope);
}

WorkerNavigator::WorkerNavigator(WorkerGlobalScope& global_scope)
    : PlatformObject(global_scope.realm())
{
}

WorkerNavigator::~WorkerNavigator() = default;

void WorkerNavigator::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WorkerNavigator);
}

}
