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
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Module);

WebIDL::ExceptionOr<GC::Ref<Module>> Module::construct_impl(JS::Realm& realm, GC::Root<WebIDL::BufferSource>& bytes)
{
    auto& vm = realm.vm();

    auto stable_bytes_or_error = WebIDL::get_buffer_source_copy(bytes->raw_object());
    if (stable_bytes_or_error.is_error()) {
        VERIFY(stable_bytes_or_error.error().code() == ENOMEM);
        return vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
    }
    auto stable_bytes = stable_bytes_or_error.release_value();

    auto compiled_module = TRY(Detail::compile_a_webassembly_module(vm, move(stable_bytes)));
    return realm.create<Module>(realm, move(compiled_module));
}

// https://webassembly.github.io/threads/js-api/index.html#dom-module-imports
WebIDL::ExceptionOr<Vector<ModuleImportDescriptor>> Module::imports(JS::VM&, GC::Ref<Module> module_object)
{
    // 1. Let module be moduleObject.[[Module]].
    // 2. Let imports be « ».
    Vector<ModuleImportDescriptor> import_objects;

    // 3. For each (moduleName, name, type) of module_imports(module),
    auto& imports = module_object->m_compiled_module->module->import_section().imports();
    import_objects.ensure_capacity(imports.size());
    for (auto& import : imports) {
        // 3.1. Let kind be the string value of the extern type type.
        auto const kind = import.description().visit(
            [](Wasm::TypeIndex) { return Bindings::ImportExportKind::Function; },
            [](Wasm::TableType) { return Bindings::ImportExportKind::Table; },
            [](Wasm::MemoryType) { return Bindings::ImportExportKind::Memory; },
            [](Wasm::GlobalType) { return Bindings::ImportExportKind::Global; },
            [](Wasm::FunctionType) { return Bindings::ImportExportKind::Function; });

        // 3.2. Let obj be «[ "module" → moduleName, "name" → name, "kind" → kind ]».
        ModuleImportDescriptor descriptor {
            .module = String::from_utf8_with_replacement_character(import.module()),
            .name = String::from_utf8_with_replacement_character(import.name()),
            .kind = kind,
        };

        // 3.3. Append obj to imports.
        import_objects.append(move(descriptor));
    }
    // 4. Return imports.
    return import_objects;
}

Module::Module(JS::Realm& realm, NonnullRefPtr<Detail::CompiledWebAssemblyModule> compiled_module)
    : Bindings::PlatformObject(realm)
    , m_compiled_module(move(compiled_module))
{
}

void Module::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Module, WebAssembly.Module);
    Base::initialize(realm);
}

}
