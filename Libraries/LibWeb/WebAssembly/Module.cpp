/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
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
        if (import.description().has<Wasm::TagType>()) {
            dbgln("Not yet implemented: importing tags");
            continue;
        }

        // 3.1. Let kind be the string value of the extern type type.
        auto const kind = import.description().visit(
            [](Wasm::TypeIndex) { return Bindings::ImportExportKind::Function; },
            [](Wasm::TableType) { return Bindings::ImportExportKind::Table; },
            [](Wasm::MemoryType) { return Bindings::ImportExportKind::Memory; },
            [](Wasm::GlobalType) { return Bindings::ImportExportKind::Global; },
            [](Wasm::FunctionType) { return Bindings::ImportExportKind::Function; },
            [](Wasm::TagType) -> Bindings::ImportExportKind { TODO(); });

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

// https://webassembly.github.io/threads/js-api/index.html#dom-module-exports
WebIDL::ExceptionOr<Vector<ModuleExportDescriptor>> Module::exports(JS::VM&, GC::Ref<Module> module_object)
{
    // 1. Let module be moduleObject.[[Module]].
    // 2. Let exports be « ».
    Vector<ModuleExportDescriptor> export_objects;

    // 3. For each (name, type) of module_exports(module),
    auto& exports = module_object->m_compiled_module->module->export_section().entries();
    export_objects.ensure_capacity(exports.size());
    for (auto& entry : exports) {
        if (entry.description().has<Wasm::TagIndex>()) {
            dbgln("Not yet implemented: exporting tags");
            continue;
        }

        // 3.1. Let kind be the string value of the extern type type.
        auto const kind = entry.description().visit(
            [](Wasm::FunctionIndex) { return Bindings::ImportExportKind::Function; },
            [](Wasm::TableIndex) { return Bindings::ImportExportKind::Table; },
            [](Wasm::MemoryIndex) { return Bindings::ImportExportKind::Memory; },
            [](Wasm::GlobalIndex) { return Bindings::ImportExportKind::Global; },
            [](Wasm::TagIndex) -> Bindings::ImportExportKind { TODO(); });
        // 3.2. Let obj be «[ "name" → name, "kind" → kind ]».
        ModuleExportDescriptor descriptor {
            .name = String::from_utf8_with_replacement_character(entry.name()),
            .kind = kind,
        };
        // 3.3. Append obj to exports.
        export_objects.append(move(descriptor));
    }
    // 4. Return exports.
    return export_objects;
}

// https://webassembly.github.io/threads/js-api/index.html#dom-module-customsections
WebIDL::ExceptionOr<GC::RootVector<GC::Ref<JS::ArrayBuffer>>> Module::custom_sections(JS::VM& vm, GC::Ref<Module> module_object, String section_name)
{
    // 1. Let bytes be moduleObject.[[Bytes]].
    // 2. Let customSections be « ».
    GC::RootVector<GC::Ref<JS::ArrayBuffer>> array_buffers { vm.heap() };

    // 3. For each custom section customSection of bytes, interpreted according to the module grammar,
    auto& custom_sections = module_object->m_compiled_module->module->custom_sections();
    for (auto& section : custom_sections) {
        // 3.1. Let name be the name of customSection, decoded as UTF-8.
        // 3.2. Assert: name is not failure (moduleObject.[[Module]] is valid).
        auto name = MUST(String::from_utf8(section.name().bytes()));
        // 3.3. If name equals sectionName as string values,
        if (section_name == name) {
            // 3.3.1. Append a new ArrayBuffer containing a copy of the bytes in bytes for the range matched by this customsec production to customSections.
            array_buffers.append(JS::ArrayBuffer::create(module_object->realm(), section.contents()));
        }
    }
    // 4. Return customSections.
    return array_buffers;
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
