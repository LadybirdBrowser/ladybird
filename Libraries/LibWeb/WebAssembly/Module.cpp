/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Module.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Module);

static Optional<Bindings::ImportExportKind> import_export_kind(Wasm::Runtime::ExternKind kind)
{
    switch (kind) {
    case Wasm::Runtime::ExternKind::Function:
        return Bindings::ImportExportKind::Function;
    case Wasm::Runtime::ExternKind::Table:
        return Bindings::ImportExportKind::Table;
    case Wasm::Runtime::ExternKind::Memory:
        return Bindings::ImportExportKind::Memory;
    case Wasm::Runtime::ExternKind::Global:
        return Bindings::ImportExportKind::Global;
    case Wasm::Runtime::ExternKind::Tag:
        return {};
    }
    VERIFY_NOT_REACHED();
}

GC::Ref<Module> Module::create()
{
    return GC::Heap::the().allocate<Module>();
}

WebIDL::ExceptionOr<GC::Ref<Module>> Module::create(JS::Realm& realm, WebIDL::BufferSource bytes)
{
    auto& vm = realm.vm();

    auto stable_bytes_or_error = WebIDL::get_buffer_source_copy(bytes);
    if (stable_bytes_or_error.is_error()) {
        VERIFY(stable_bytes_or_error.error().code() == ENOMEM);
        return vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
    }
    auto stable_bytes = stable_bytes_or_error.release_value();

    auto compiled_module = TRY(Detail::compile_a_webassembly_module(realm, stable_bytes));
    return GC::Heap::the().allocate<Module>(move(compiled_module), move(stable_bytes));
}

Module::Module(NonnullRefPtr<Detail::CompiledWebAssemblyModule> compiled_module, ByteBuffer bytes)
    : m_compiled_module(move(compiled_module))
    , m_bytes(move(bytes))
{
}

// https://webassembly.github.io/threads/js-api/index.html#dom-module-imports
WebIDL::ExceptionOr<Vector<Bindings::ModuleImportDescriptor>> Module::imports(GC::Ref<Module> module_object)
{
    // 1. Let module be moduleObject.[[Module]].
    // 2. Let imports be « ».
    Vector<Bindings::ModuleImportDescriptor> import_objects;

    // 3. For each (moduleName, name, type) of module_imports(module),
    auto& module = *module_object->m_compiled_module->module;
    auto import_count = module.import_count();
    import_objects.ensure_capacity(import_count);
    for (size_t index = 0; index < import_count; ++index) {
        auto import = module.import(index);
        // 3.1. Let kind be the string value of the extern type type.
        auto kind = import_export_kind(import.type.kind);
        if (!kind.has_value()) {
            dbgln("Not yet implemented: importing tags");
            continue;
        }

        // 3.2. Let obj be «[ "module" → moduleName, "name" → name, "kind" → kind ]».
        Bindings::ModuleImportDescriptor descriptor {
            .kind = *kind,
            .module = Utf16String::from_utf8(import.module),
            .name = Utf16String::from_utf8(import.name),
        };

        // 3.3. Append obj to imports.
        import_objects.append(move(descriptor));
    }

    // 4. Return imports.
    return import_objects;
}

// https://webassembly.github.io/threads/js-api/index.html#dom-module-exports
WebIDL::ExceptionOr<Vector<Bindings::ModuleExportDescriptor>> Module::exports(GC::Ref<Module> module_object)
{
    // 1. Let module be moduleObject.[[Module]].
    // 2. Let exports be « ».
    Vector<Bindings::ModuleExportDescriptor> export_objects;

    // 3. For each (name, type) of module_exports(module),
    auto& module = *module_object->m_compiled_module->module;
    auto export_count = module.export_count();
    export_objects.ensure_capacity(export_count);
    for (size_t index = 0; index < export_count; ++index) {
        auto entry = module.module_export(index);
        // 3.1. Let kind be the string value of the extern type type.
        auto kind = import_export_kind(entry.type.kind);
        if (!kind.has_value()) {
            dbgln("Not yet implemented: exporting tags");
            continue;
        }

        // 3.2. Let obj be «[ "name" → name, "kind" → kind ]».
        Bindings::ModuleExportDescriptor descriptor {
            .kind = *kind,
            .name = Utf16String::from_utf8(entry.name),
        };

        // 3.3. Append obj to exports.
        export_objects.append(move(descriptor));
    }

    // 4. Return exports.
    return export_objects;
}

WebIDL::ExceptionOr<Vector<ByteBuffer>> Module::custom_sections(GC::Ref<Module> module_object, Utf16String section_name)
{
    Vector<ByteBuffer> matching_sections;

    auto& module = *module_object->m_compiled_module->module;
    for (size_t index = 0; index < module.custom_section_count(); ++index) {
        auto section = module.custom_section(index);
        auto name = Utf16String::from_utf8(section.name);
        if (section_name == name)
            matching_sections.append(MUST(ByteBuffer::copy(section.contents)));
    }

    return matching_sections;
}

// https://webassembly.github.io/threads/js-api/index.html#dom-module-customsections
WebIDL::ExceptionOr<GC::RootVector<GC::Ref<JS::ArrayBuffer>>> Module::custom_sections(JS::Realm& realm, GC::Ref<Module> module_object, Utf16String section_name)
{
    // 1. Let bytes be moduleObject.[[Bytes]].
    // 2. Let customSections be « ».
    GC::RootVector<GC::Ref<JS::ArrayBuffer>> array_buffers;

    // 3. For each custom section customSection of bytes, interpreted according to the module grammar,
    auto custom_sections = TRY(Module::custom_sections(module_object, move(section_name)));
    for (auto& section : custom_sections) {
        // 3.3.1. Append a new ArrayBuffer containing a copy of the bytes in bytes for the range matched by this customsec production to customSections.
        array_buffers.append(JS::ArrayBuffer::create(realm, move(section)));
    }

    // 4. Return customSections.
    return array_buffers;
}

// https://webassembly.github.io/spec/web-api/#ref-for-serialization-steps
WebIDL::ExceptionOr<void> Module::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool for_storage, HTML::SerializationMemory&)
{
    // 1. If forStorage is true, throw a "DataCloneError" DOMException.
    if (for_storage)
        return WebIDL::DataCloneError::create("WebAssembly Module instances may not be serialized for storage"_utf16);

    // 2. Set serialized.[[Bytes]] to the sub-serialization of value.[[Bytes]].
    serialized.encode(m_bytes);

    // 3. Set serialized.[[AgentCluster]] to the current Realm’s corresponding agent cluster.
    serialized.encode(HTML::current_settings_object().agent_cluster_id());

    return {};
}

// https://webassembly.github.io/spec/web-api/#ref-for-deserialization-steps
WebIDL::ExceptionOr<void> Module::deserialization_steps(JS::Realm& realm, HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory&)
{
    // 1. Let bytes be the sub-deserialization of serialized.[[Bytes]].
    auto bytes = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, serialized));

    // 2. Set value.[[Bytes]] to bytes.
    m_bytes = move(bytes);

    // 3. If targetRealm’s corresponding agent cluster is not serialized.[[AgentCluster]], then throw a "DataCloneError" DOMException.
    auto agent_cluster = TRY(HTML::decode_or_throw_data_clone_error<Optional<u64>>(realm, serialized));
    auto target_agent_cluster = Bindings::principal_host_defined_environment_settings_object(realm).agent_cluster_id();
    if (!agent_cluster.has_value() || agent_cluster != target_agent_cluster)
        return WebIDL::DataCloneError::create("Cannot deserialize WebAssembly Module outside its agent cluster"_utf16);

    // 4. Compile a WebAssembly module from bytes and set value.[[Module]] to the result.
    m_compiled_module = TRY(Detail::compile_a_webassembly_module(realm, m_bytes));

    return {};
}

}
