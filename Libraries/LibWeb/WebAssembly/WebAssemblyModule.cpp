/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ConservativeVector.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/BindingsGlue.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/Instance.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/Tag.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebAssembly/WebAssemblyModule.h>

namespace Web::WebAssembly {

namespace {

// The fields that a WebAssembly Module Record has besides those of a Cyclic Module Record.
class WebAssemblyModuleHostData final : public GC::Cell {
    GC_CELL(WebAssemblyModuleHostData, GC::Cell);
    GC_DECLARE_ALLOCATOR(WebAssemblyModuleHostData);

public:
    using JSValueConversionIsForbidden = void;

    GC::Ptr<Instance> instance() const { return m_instance; }
    void set_instance(GC::Ref<Instance> instance) { m_instance = instance; }

    GC::Ref<WebAssembly::Module> module_source() const { return m_module_source; }

    void set_module_record(GC::Ref<JS::HostModule> module_record) { m_module_record = module_record; }

    Vector<Utf16FlyString> export_name_list();

private:
    explicit WebAssemblyModuleHostData(WebAssembly::Module& module_source)
        : m_module_source(module_source)
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_instance);
        visitor.visit(m_module_source);
        visitor.visit(m_module_record);
    }

    GC::Ptr<Instance> m_instance;                 // [[Instance]]
    GC::Ref<WebAssembly::Module> m_module_source; // [[ModuleSource]]
    GC::Ptr<JS::HostModule> m_module_record;      // [[ModuleRecord]]

    Optional<Vector<Utf16FlyString>> m_cached_export_name_list;
};

GC_DEFINE_ALLOCATOR(WebAssemblyModuleHostData);

struct WebAssemblyModuleTraits {
    static Vector<Utf16FlyString> get_exported_names(JS::HostModule&);
    static JS::ResolvedBinding resolve_export(JS::HostModule&, Utf16FlyString const& export_name);
    static JS::ThrowCompletionOr<void> initialize_environment(JS::HostModule&);
    static JS::ThrowCompletionOr<void> execute_module(JS::HostModule&, GC::Ptr<JS::PromiseCapability>);
};

constexpr JSHostModuleHooks webassembly_module_hooks = JS::make_host_module_hooks<WebAssemblyModuleTraits>();
constexpr JSHostClass webassembly_module_host_class = JS::make_host_class(JS_HOST_CLASS_MODULE, "WebAssemblyModule"sv, nullptr, &webassembly_module_hooks, nullptr, 0);

bool is_webassembly_module_record(JS::Module const& module)
{
    return JS::is_host_instance_of(module, webassembly_module_host_class);
}

WebAssemblyModuleHostData& host_data_of_webassembly_module_record(JS::Module const& module)
{
    auto* host_data = JS::host_data_if<WebAssemblyModuleHostData>(module);
    VERIFY(host_data);
    return *host_data;
}

}

// https://webassembly.github.io/esm-integration/js-api/index.html#parse-a-webassembly-module
JS::ThrowCompletionOr<GC::Ref<JS::HostModule>> parse_a_webassembly_module(ByteBuffer bytes, JS::Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    auto& vm = realm.vm();

    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    auto stable_bytes = MUST(ByteBuffer::copy(bytes));

    // 2. Compile the WebAssembly module stableBytes and store the result as module.
    // 3. If module is error, throw a CompileError exception.
    // NOTE:  When integrating with the JS String Builtins proposal, builtinSetNames should be passed in the following
    //        step as « "js-string" » and importedStringModule as null.
    auto module = TRY(Detail::compile_a_webassembly_module(realm, stable_bytes));

    // 4. Construct a WebAssembly module object from module and bytes, and let module be the result.
    auto module_object = GC::Heap::the().allocate<WebAssembly::Module>(module, move(bytes), Vector<Utf16String> {}, OptionalNone {});

    // 5. Let requestedModules be a set.
    HashTable<ByteString> requested_modules;

    // 6. For each (moduleName, name, type) in module_imports(module.[[Module]]),
    auto& compiled_module = *module_object->compiled_module()->module;
    for (size_t index = 0; index < compiled_module.import_count(); ++index) {
        auto entry = compiled_module.import(index);

        // 1. If moduleName starts with the prefix "wasm-js:",
        if (entry.module.starts_with("wasm-js:"sv)) {
            // 1. Throw a LinkError exception.
            return vm.throw_completion<LinkError>("Import with invalid module name"sv);
        }

        // 2. If name starts with the prefix "wasm:" or "wasm-js:",
        if (entry.name.starts_with("wasm:"sv) || entry.name.starts_with("wasm-js:"sv)) {
            // 1. Throw a LinkError exception.
            return vm.throw_completion<LinkError>("Import with invalid name"sv);
        }

        // NOTE: The following step only applies when integrating with the JS String Builtins proposal.
        // FIXME: 3. If Find a builtin with (moduleName, name, type) and builtins module.[[BuiltinSets]] is not null,
        //           then continue.

        // 4. Append moduleName to requestedModules.
        requested_modules.set(entry.module);
    }

    // 7. For each (name, type) in module_exports(module.[[Module]])
    for (size_t index = 0; index < compiled_module.export_count(); ++index) {
        auto entry = compiled_module.module_export(index);

        // 1. If name starts with the prefix "wasm:" or "wasm-js:",
        if (entry.name.starts_with("wasm:"sv) || entry.name.starts_with("wasm-js:"sv)) {
            // 1. Throw a LinkError exception.
            return vm.throw_completion<LinkError>("Export with invalid name"sv);
        }
    }

    // 8. Let moduleRecord be { [[Instance]]: ~empty~, [[Realm]]: realm, [[Environment]]: ~empty~,
    //                          [[Namespace]]: ~empty~, [[ModuleSource]]: module, [[HostDefined]]: hostDefined,
    //                          [[Status]]: "new", [[EvaluationError]]: undefined, [[DFSIndex]]: undefined,
    //                          [[DFSAncestorIndex]]: undefined, [[RequestedModules]]: requestedModules,
    //                          [[LoadedModules]]: « », [[CycleRoot]]: ~empty~, [[HasTLA]]: false,
    //                          [[AsyncEvaluation]]: false, [[TopLevelCapability]]: ~empty~ [[AsyncParentModules]]: « »,
    //                          [[PendingAsyncDependencies]]: ~empty~, }.
    AK::Vector<JS::ModuleRequest> module_requests;
    for (auto const& module_name : requested_modules) {
        module_requests.append(JS::ModuleRequest { Utf16FlyString::from_utf8(module_name), {} });
    }
    auto host_data = realm.heap().allocate<WebAssemblyModuleHostData>(module_object);
    auto module_record = JS::HostModule::create(realm, webassembly_module_host_class, filename, move(module_requests), host_defined, host_data);

    // 9. Set module.[[ModuleRecord]] to moduleRecord.
    host_data->set_module_record(module_record);

    // 10. Return moduleRecord.
    return module_record;
}

// https://webassembly.github.io/esm-integration/js-api/index.html#export-name-list
Vector<Utf16FlyString> WebAssemblyModuleHostData::export_name_list()
{
    // AD-HOC: Return cached export name list if available
    if (m_cached_export_name_list.has_value())
        return m_cached_export_name_list.value();

    // 1. Let module be record’s [[ModuleSource]] internal slot.
    auto module = m_module_source;

    // 2. Let exports be an empty list.
    Vector<Utf16FlyString> exports;

    // 3. For each(name, type) in module_exports(module.[[Module]])
    auto& compiled_module = *module->compiled_module()->module;
    for (size_t index = 0; index < compiled_module.export_count(); ++index) {
        // 1. Append name to the end of exports.
        exports.append(Utf16FlyString::from_utf8(compiled_module.module_export(index).name));
    }

    // AD-HOC: Cache exports
    m_cached_export_name_list = exports;

    // 4. Return exports.
    return exports;
}

// https://webassembly.github.io/esm-integration/js-api/index.html#get-exported-names
Vector<Utf16FlyString> WebAssemblyModuleTraits::get_exported_names(JS::HostModule& record)
{
    // 1. Let record be this WebAssembly Module Record.
    // 2. Return the export name list of record.
    return host_data_of_webassembly_module_record(record).export_name_list();
}

// https://webassembly.github.io/esm-integration/js-api/index.html#resolve-export
JS::ResolvedBinding WebAssemblyModuleTraits::resolve_export(JS::HostModule& record, Utf16FlyString const& export_name)
{
    // 1. Let record be this WebAssembly Module Record.
    // 2. If the export name list of record contains exportName, return { [[Module]]: record, [[BindingName]]: exportName }.
    if (host_data_of_webassembly_module_record(record).export_name_list().contains_slow(export_name)) {
        return JS::ResolvedBinding { JS::ResolvedBinding::Type::BindingName, &record, export_name };
    }

    // 3. Otherwise, return null.
    return JS::ResolvedBinding::null();
}

// https://webassembly.github.io/esm-integration/js-api/index.html#module-declaration-environment-setup
JS::ThrowCompletionOr<void> WebAssemblyModuleTraits::initialize_environment(JS::HostModule& record)
{
    auto& vm = record.vm();

    // 1. Let record be this WebAssembly Module Record.
    // 2. Let env be NewModuleEnvironment(null).
    auto env = JS::new_module_environment(nullptr);

    // 3. Set record.[[Environment]] to env.
    record.set_environment(env);

    // 4. For each name in the export name list of record,
    for (auto const& name : host_data_of_webassembly_module_record(record).export_name_list()) {
        // 1. Perform !env.CreateImmutableBinding(name, true).
        MUST(env->create_immutable_binding(vm, name, true));
    }

    return {};
}

// https://webassembly.github.io/esm-integration/js-api/index.html#module-execution
JS::ThrowCompletionOr<void> WebAssemblyModuleTraits::execute_module(JS::HostModule& record, GC::Ptr<JS::PromiseCapability> capability)
{
    auto& vm = record.vm();

    // 1. Assert: promiseCapability was not provided.
    VERIFY(!capability);

    // 2. Let record be this WebAssembly Module Record.
    auto& record_host_data = host_data_of_webassembly_module_record(record);
    auto& realm = record.realm();

    // 3. Let module be record.[[ModuleSource]].[[Module]].
    auto& module = *record_host_data.module_source()->compiled_module()->module;
    auto& store = Detail::WebAssemblyCache::the().store();

    // 4. Let imports be « ».
    // NB: The host functions and globals made for the imports are referenced from here only until the instance
    //     holds them, and converting values runs user code (valueOf) that may collect garbage.
    GC::ConservativeVector<Wasm::Runtime::Extern> imports;
    size_t imported_function_count = 0;

    // 5. For each (importedModuleName, name, importtype) in module_imports(module),
    for (size_t import_index = 0; import_index < module.import_count(); ++import_index) {
        auto entry = module.import(import_index);

        // NOTE: The following step only applies when integrating with the JS String Builtins proposal.
        // FIXME: 1. If Find a builtin with (importedModuleName, name) and builtins module.[[BuiltinSets]] is not null, then continue.

        // 2. Let importedModule be GetImportedModule(record, importedModuleName).
        auto imported_module = record.get_imported_module(JS::ModuleRequest { Utf16FlyString::from_utf8(entry.module) });

        // 3. Let resolution be importedModule.ResolveExport(name).
        auto resolution = imported_module->resolve_export(vm, Utf16FlyString::from_utf8(entry.name));

        // 4. Assert: resolution is a ResolvedBinding Record, as validated during environment initialization.
        VERIFY(resolution.is_valid());

        // 5. Let resolvedModule be resolution.\[[Module]].
        auto resolved_module = resolution.module;

        // 6. Let resolvedName be resolution.[[BindingName]].
        auto resolved_name = resolution.export_name;

        // 7. If resolvedModule is a WebAssembly Module Record,
        if (is_webassembly_module_record(*resolved_module)) {
            auto& resolved_module_host_data = host_data_of_webassembly_module_record(*resolved_module);

            // 1. If resolvedModule.[[Instance]] is ~empty~, throw a {LinkError} exception.
            if (!resolved_module_host_data.instance()) {
                return vm.throw_completion<LinkError>("Module has not been instantiated"sv);
            }

            // 2. Assert: resolvedModule.[[Instance]] is a WebAssembly Instance object.
            // 3. Assert: resolvedModule.[[ModuleSource]] is a WebAssembly Module object.
            // 4. Let module be resolvedModule.[[ModuleSource]].[[Module]].
            auto& resolved_instance = *resolved_module_host_data.instance();

            // 5. Let externval be instance_export(resolvedModule.[[Instance]], resolvedName).
            // https://webassembly.github.io/spec/core/appendix/embedding.html#embed-instance-export
            auto resolved_name_utf8 = resolved_name.view().to_utf8_but_should_be_ported_to_utf16();
            auto externval = store.find_export(resolved_instance.instance_address(), resolved_name_utf8.bytes_as_string_view());

            // 6. Assert: externval is not error.
            VERIFY(externval.has_value());

            // 7. Assert: module_exports(module) contains an element (resolvedName, type).
            // 8. Let externtype be the value of type for the element (resolvedName, type) in module_exports(module).
            // 9. If importtype is not an extern subtype of externtype, throw a LinkError exception.
            // NB: Instantiation checks the type of every import, and throws that LinkError.

            // 10. Append externval to imports.
            imports.append(*externval);
        }

        // 8. Otherwise,
        else {
            // 1. Let env be resolvedModule.[[Environment]].
            auto env = resolved_module->environment();

            // 2. Let v be ?env.GetBindingValue(resolvedName, true).
            auto v = TRY(env->get_binding_value(vm, resolved_name, true));

            switch (entry.type.kind) {
            // 3. If importtype is of the form func functype,
            case Wasm::Runtime::ExternKind::Function: {
                // 1. If IsCallable(v) is false, throw a LinkError exception.
                if (!v.is_function())
                    return vm.throw_completion<LinkError>(JS::ErrorType::NotAFunction, v);
                auto& function = v.as_function();

                Wasm::Runtime::Entity funcaddr = nullptr;
                // 2. If v has a [[FunctionAddress]] internal slot, and therefore is an Exported Function,
                if (auto exported_address = Detail::exported_function_address(function); exported_address.has_value()) {
                    // 1. Let funcaddr be the value of v’s [[FunctionAddress]] internal slot.
                    funcaddr = *exported_address;
                }
                // 3. Otherwise,
                else {
                    // 1. Create a host function from v and functype, and let funcaddr be the result.
                    // 2. Let index be the number of external functions in imports, defining the index of the host function funcaddr.
                    funcaddr = Detail::create_host_function(vm, function, Detail::function_type_in_store(module, entry.type.type_index), imported_function_count);
                }

                // 4. Let externfunc be the external value func funcaddr.
                // 5. Append externfunc to imports.
                imports.append({ Wasm::Runtime::ExternKind::Function, funcaddr });
                break;
            }

            // 4. If importtype is of the form global mut valtype,
            case Wasm::Runtime::ExternKind::Global: {
                auto valtype = store.canonical_value_type(module, entry.type.value_type);
                auto is_mutable = entry.type.is_mutable;

                // 1. Let store be the surrounding agent’s associated store.
                // 2. If v implements Global,
                Wasm::Runtime::Entity globaladdr = nullptr;
                if (auto* global = Bindings::global_from_value(v)) {
                    // 1. Let globaladdr be v.[[Global]].
                    globaladdr = global->address();

                    // 2. Let targetmut valuetype be global_type(store, globaladdr).
                    bool target_is_mutable = false;
                    (void)store.global_type(globaladdr, target_is_mutable);

                    // 3. If mut is const and targetmut is var, throw a LinkError exception.
                    if (!is_mutable && target_is_mutable) {
                        return vm.throw_completion<LinkError>("Mutable globals are not supported for immutable imports"sv);
                    }
                }

                // 3. Otherwise,
                else {
                    // AD-HOC: If valtype is i64 and v is a Number, throw a LinkError exception.
                    if (valtype.kind == Wasm::Runtime::ValueKind::I64 && v.is_number()) {
                        return vm.throw_completion<LinkError>("Import resolution attempted to cast a Number to a BigInteger"sv);
                    }

                    // AD-HOC: If valtype is not i64 and v is a BigInt, throw a LinkError exception.
                    if (valtype.kind != Wasm::Runtime::ValueKind::I64 && v.is_bigint()) {
                        return vm.throw_completion<LinkError>("Import resolution attempted to cast a BigInteger to a Number"sv);
                    }

                    // 1. If valtype is v128, throw a LinkError exception.
                    if (valtype.kind == Wasm::Runtime::ValueKind::V128) {
                        return vm.throw_completion<LinkError>("V128 is not supported as a global value type"sv);
                    }

                    // 2. If mut is var, throw a LinkError exception.
                    if (is_mutable) {
                        return vm.throw_completion<LinkError>("Variable global value types are not supported"sv);
                    }

                    // 3. Let value be ?ToWebAssemblyValue(v, valtype).
                    auto value = TRY(Detail::to_webassembly_value(vm, v, valtype));

                    // 4. Let(store, globaladdr) be global_alloc(store, mut valtype, value).
                    // 5. Set the surrounding agent’s associated store to store.
                    globaladdr = store.allocate_global(valtype, is_mutable, value);
                }

                // 4. Let externglobal be global globaladdr.
                // 5. Append externglobal to imports.
                imports.append({ Wasm::Runtime::ExternKind::Global, globaladdr });
                break;
            }

            // 5. If importtype is of the form mem memtype,
            case Wasm::Runtime::ExternKind::Memory: {
                // 1. If v does not implement Memory, throw a LinkError exception.
                auto* memory = Bindings::memory_from_value(v);
                if (!memory) {
                    return vm.throw_completion<LinkError>("Expected an instance of WebAssembly.Memory for a memory import"sv);
                }

                // 2. Let externmem be the external value mem v.[[Memory]].
                // 3. Append externmem to imports.
                imports.append({ Wasm::Runtime::ExternKind::Memory, memory->address() });
                break;
            }

            // 6. If importtype is of the form table tabletype,
            case Wasm::Runtime::ExternKind::Table: {
                // 1. If v does not implement Table, throw a LinkError exception.
                auto* table = Bindings::table_from_value(v);
                if (!table) {
                    return vm.throw_completion<LinkError>("Expected an instance of WebAssembly.Table for a table import"sv);
                }

                // 2. Let tableaddr be v.[[Table]].
                // 3. Let externtable be the external value table tableaddr.
                // 4. Append externtable to imports.
                imports.append({ Wasm::Runtime::ExternKind::Table, table->address() });
                break;
            }

            // 7. If importtype is of the form tag attribute functype,
            case Wasm::Runtime::ExternKind::Tag: {
                // 1. Assert: attribute is exception.
                // 2. If v does not implement Tag, throw a LinkError exception.
                auto* tag = Bindings::tag_from_value(v);
                if (!tag)
                    return vm.throw_completion<LinkError>("Expected an instance of WebAssembly.Tag for a tag import"sv);

                // 3. Let tagaddr be v.[[Address]].
                // 4. Let externtag be the external value tag tagaddr.
                // 5. Append externtag to imports.
                imports.append({ Wasm::Runtime::ExternKind::Tag, tag->address() });
                break;
            }
            }
        }

        if (entry.type.kind == Wasm::Runtime::ExternKind::Function)
            ++imported_function_count;
    }

    // 6. Instantiate the core of a WebAssembly module module with imports, and let instance be the result.
    auto instance_address = TRY(Detail::instantiate_core_module(vm, module, imports));

    // 7. Set record.[[Instance]] to instance.
    auto instance = Instance::create(realm, instance_address);
    record_host_data.set_instance(instance);

    // 8. For each (name, externtype) of module_exports(module),
    for (auto const& entry : store.instance_exports(instance_address)) {
        // 1. If externtype is of the form global mut globaltype,
        if (entry.value.kind == Wasm::Runtime::ExternKind::Global) {
            // 1. Assert: externval is of the form global globaladdr.
            // 2. Let global globaladdr be externval.
            // 3. Let global_value be global_read(store, globaladdr).
            auto globaladdr = entry.value.entity.ptr();
            bool is_mutable = false;
            auto type = store.global_type(globaladdr, is_mutable);

            // 4. If globaltype is not v128,
            if (type.kind != Wasm::Runtime::ValueKind::V128) {
                // NOTE: The condition above leaves unsupported JS values as uninitialized in TDZ and therefore as a
                //       reference error on access. When integrating with shared globals, they may be excluded here
                //       similarly to v128 above.

                // 1. Perform !record.[[Environment]].InitializeBinding(name, ToJSValue(global_value)).
                auto value = TRY(Detail::to_js_value(vm, store.global_get(globaladdr), type));
                MUST(record.environment()->initialize_binding(vm, Utf16FlyString::from_utf8(entry.name), value, JS::Environment::InitializeBindingHint::Normal));

                // FIXME: 2. If mut is var, then associate all future mutations of globaladdr with the ECMA-262 binding record
                //        for name in record.[[Environment]], such that record.[[Environment]].GetBindingValue(resolution.[[BindingName]], true)
                //        always returns ToJSValue(global_read(store, globaladdr)) for the current surrounding agent’s associated store store.
            }
        }

        // 2. Otherwise,
        else {
            // 1. Perform !record.[[Environment]].InitializeBinding(name, !Get(instance.[[Exports]], name)).
            auto name = Utf16FlyString::from_utf8(entry.name);
            MUST(Bindings::initialize_webassembly_export_binding(realm, *record.environment(), name, instance));
        }
    }

    // NOTE: The linking semantics here for Wasm to Wasm modules are identical to the WebAssembly JS API semantics as if
    //       passing the the exports object as the imports object in instantiation. When linking Wasm module imports to
    //       JS module exports, the JS API semantics are exactly followed as well. It is only in the case of importing
    //       Wasm from JS that WebAssembly.Global unwrapping is observable on the WebAssembly Module Record Environment
    //       Record.

    return {};
}

}
