/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ModulePrototype.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Module);

WebIDL::ExceptionOr<GC::Ref<Module>> Module::construct_impl(JS::Realm& realm, GC::Handle<WebIDL::BufferSource>& bytes)
{
    auto& vm = realm.vm();

    auto compiled_module = TRY(Detail::parse_module(vm, bytes->raw_object()));
    return vm.heap().allocate<Module>(realm, realm, move(compiled_module));
}

Module::Module(JS::Realm& realm, NonnullRefPtr<Detail::CompiledWebAssemblyModule> compiled_module)
    : Bindings::PlatformObject(realm)
    , m_compiled_module(move(compiled_module))
{
}

void Module::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Module, WebAssembly.Module);
}

}
