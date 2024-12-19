/*
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Error.h"
#include <LibJS/Runtime/GlobalObject.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(CompileError);

GC::Ref<CompileError> CompileError::create(JS::Realm& realm)
{
    return realm.heap().allocate<CompileError>(realm.intrinsics().error_prototype());
}

GC::Ref<CompileError> CompileError::create(JS::Realm& realm, StringView message)
{
    auto& vm = realm.vm();
    auto error = CompileError::create(realm);
    u8 attr = JS::Attribute::Writable | JS::Attribute::Configurable;
    error->define_direct_property(vm.names.message, JS::PrimitiveString::create(vm, message), attr);
    return error;
}

CompileError::CompileError(JS::Object& prototype)
    : JS::Error(prototype)
{
}

GC_DEFINE_ALLOCATOR(LinkError);

GC::Ref<LinkError> LinkError::create(JS::Realm& realm)
{
    return realm.heap().allocate<LinkError>(realm.intrinsics().error_prototype());
}

GC::Ref<LinkError> LinkError::create(JS::Realm& realm, StringView message)
{
    auto& vm = realm.vm();
    auto error = LinkError::create(realm);
    u8 attr = JS::Attribute::Writable | JS::Attribute::Configurable;
    error->define_direct_property(vm.names.message, JS::PrimitiveString::create(vm, message), attr);
    return error;
}

LinkError::LinkError(JS::Object& prototype)
    : JS::Error(prototype)
{
}

GC_DEFINE_ALLOCATOR(RuntimeError);

GC::Ref<RuntimeError> RuntimeError::create(JS::Realm& realm)
{
    return realm.heap().allocate<RuntimeError>(realm.intrinsics().error_prototype());
}

GC::Ref<RuntimeError> RuntimeError::create(JS::Realm& realm, StringView message)
{
    auto& vm = realm.vm();
    auto error = RuntimeError::create(realm);
    u8 attr = JS::Attribute::Writable | JS::Attribute::Configurable;
    error->define_direct_property(vm.names.message, JS::PrimitiveString::create(vm, message), attr);
    return error;
}

RuntimeError::RuntimeError(JS::Object& prototype)
    : JS::Error(prototype)
{
}

} // namespace Web::WebAssembly
