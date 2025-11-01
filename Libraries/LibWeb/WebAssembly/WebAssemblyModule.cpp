/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/Instance.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebAssembly/WebAssemblyModule.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(WebAssemblyModule);

WebAssemblyModule::WebAssemblyModule(JS::Realm& realm, StringView filename, WebAssembly::Module& module_source,
    JS::Script::HostDefined* host_defined, Vector<JS::ModuleRequest> requested_modules)
    : CyclicModule(realm, filename, false, move(requested_modules), host_defined)
    , m_module_source(module_source)
{
}

WebAssemblyModule::~WebAssemblyModule() = default;

void WebAssemblyModule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_instance);
    visitor.visit(m_module_source);
    visitor.visit(m_module_record);
}

// https://webassembly.github.io/esm-integration/js-api/index.html#parse-a-webassembly-module
JS::ThrowCompletionOr<GC::Ref<WebAssemblyModule>> WebAssemblyModule::parse(ByteBuffer bytes, JS::Realm& realm, StringView filename, JS::Script::HostDefined* host_defined)
{
    auto& vm = realm.vm();

    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    auto stable_bytes
        = MUST(ByteBuffer::create_uninitialized(bytes.size()));
    bytes.bytes().copy_to(stable_bytes);

    // 2. Compile the WebAssembly module stableBytes and store the result as module.
    // 3. If module is error, throw a CompileError exception.
    // NOTE:  When integrating with the JS String Builtins proposal, builtinSetNames should be passed in the following
    //        step as « "js-string" » and importedStringModule as null.
    auto module = TRY(Detail::compile_a_webassembly_module(vm, stable_bytes));

    // 4. Construct a WebAssembly module object from module and bytes, and let module be the result.
    auto module_object = realm.create<WebAssembly::Module>(realm, module);

    // 5. Let requestedModules be a set.
    HashTable<ByteString> requested_modules;

    // 6. For each (moduleName, name, type) in module_imports(module.[[Module]]),
    auto const& imports = module_object->compiled_module()->module->import_section().imports();
    for (auto const& entry : imports) {
        // 1. If moduleName starts with the prefix "wasm-js:",
        if (entry.module().starts_with("wasm-js:"sv)) {
            // 1. Throw a LinkError exception.
            return vm.throw_completion<LinkError>("Import with invalid module name"sv);
        }

        // 2. If name starts with the prefix "wasm:" or "wasm-js:",
        if (entry.name().starts_with("wasm:"sv) || entry.name().starts_with("wasm-js:"sv)) {
            // 1. Throw a LinkError exception.
            return vm.throw_completion<LinkError>("Import with invalid name"sv);
        }

        // NOTE: The following step only applies when integrating with the JS String Builtins proposal.
        // FIXME: 3. If Find a builtin with (moduleName, name, type) and builtins module.[[BuiltinSets]] is not null,
        //           then continue.

        // 4. Append moduleName to requestedModules.
        requested_modules.set(entry.module());
    }

    // 7. For each (name, type) in module_exports(module.[[Module]])
    auto const& exports = module_object->compiled_module()->module->export_section().entries();
    for (auto const& entry : exports) {
        // 1. If name starts with the prefix "wasm:" or "wasm-js:",
        if (entry.name().starts_with("wasm:"sv) || entry.name().starts_with("wasm-js:"sv)) {
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
    auto module_record = realm.create<WebAssemblyModule>(realm, filename, module_object, host_defined, module_requests);

    // 9. Set module.[[ModuleRecord]] to moduleRecord.
    module_record->m_module_record = module_record;

    // 10. Return moduleRecord.
    return module_record;
}

// https://webassembly.github.io/esm-integration/js-api/index.html#export-name-list
Vector<Utf16FlyString> WebAssemblyModule::export_name_list()
{
    // AD-HOC: Return cached export name list if available
    if (m_cached_export_name_list.has_value())
        return m_cached_export_name_list.value();

    // 1. Let module be record’s [[ModuleSource]] internal slot.
    auto module = m_module_source;

    // 2. Let exports be an empty list.
    Vector<Utf16FlyString> exports;

    // 3. For each(name, type) in module_exports(module.[[Module]])
    auto module_exports = module->compiled_module()->module->export_section().entries();
    for (auto const& entry : module_exports) {
        // 1. Append name to the end of exports.
        exports.append(Utf16FlyString::from_utf8(entry.name()));
    }

    // AD-HOC: Cache exports
    m_cached_export_name_list = exports;

    // 4. Return exports.
    return exports;
}

// https://webassembly.github.io/esm-integration/js-api/index.html#get-exported-names
Vector<Utf16FlyString> WebAssemblyModule::get_exported_names(JS::VM&, HashTable<Module const*>&)
{
    // 1. Let record be this WebAssembly Module Record.
    auto* record = this;

    // 2. Return the export name list of record.
    return record->export_name_list();
}

// https://webassembly.github.io/esm-integration/js-api/index.html#resolve-export
JS::ResolvedBinding WebAssemblyModule::resolve_export(JS::VM&, Utf16FlyString const& export_name, Vector<JS::ResolvedBinding>)
{
    // 1. Let record be this WebAssembly Module Record.
    auto* record = this;

    // 2. If the export name list of record contains exportName, return { [[Module]]: record, [[BindingName]]: exportName }.
    if (export_name_list().contains_slow(export_name)) {
        return JS::ResolvedBinding { JS::ResolvedBinding::Type::BindingName, record, export_name };
    }

    // 3. Otherwise, return null.
    return JS::ResolvedBinding::null();
}

// https://webassembly.github.io/esm-integration/js-api/index.html#module-declaration-environment-setup
JS::ThrowCompletionOr<void> WebAssemblyModule::initialize_environment(JS::VM& vm)
{
    // 1. Let record be this WebAssembly Module Record.
    auto* record = this;

    // 2. Let env be NewModuleEnvironment(null).
    auto env = vm.heap().allocate<JS::ModuleEnvironment>(nullptr);

    // 3. Set record.[[Environment]] to env.
    record->set_environment(env);

    // 4. For each name in the export name list of record,
    for (auto const& name : export_name_list()) {
        // 1. Perform !env.CreateImmutableBinding(name, true).
        MUST(env->create_immutable_binding(vm, name, true));
    }

    return {};
}

// https://webassembly.github.io/esm-integration/js-api/index.html#module-execution
JS::ThrowCompletionOr<void> WebAssemblyModule::execute_module(JS::VM& vm, GC::Ptr<JS::PromiseCapability> capability)
{
    auto& cache = Detail::get_cache(*vm.current_realm());

    // 1. Assert: promiseCapability was not provided.
    VERIFY(!capability);

    // 2. Let record be this WebAssembly Module Record.
    auto* record = this;

    // 3. Let module be record.[[ModuleSource]].[[Module]].
    auto module = record->m_module_source->compiled_module();

    // 4. Let imports be « ».
    Vector<Wasm::ExternValue> imports;

    // 5. For each (importedModuleName, name, importtype) in module_imports(module),
    for (auto const& entry : module->module->import_section().imports()) {
        // NOTE: The following step only applies when integrating with the JS String Builtins proposal.
        // FIXME: 1. If Find a builtin with (importedModuleName, name) and builtins module.[[BuiltinSets]] is not null, then continue.

        // 2. Let importedModule be GetImportedModule(record, importedModuleName).
        auto imported_module = record->get_imported_module(JS::ModuleRequest { Utf16FlyString::from_utf8(entry.module()) });

        // 3. Let resolution be importedModule.ResolveExport(name).
        auto resolution = imported_module->resolve_export(vm, Utf16FlyString::from_utf8(entry.name()));

        // 4. Assert: resolution is a ResolvedBinding Record, as validated during environment initialization.
        VERIFY(resolution.is_valid());

        // 5. Let resolvedModule be resolution.\[[Module]].
        auto resolved_module = resolution.module;

        // 6. Let resolvedName be resolution.[[BindingName]].
        auto resolved_name = resolution.export_name;

        // 7. If resolvedModule is a WebAssembly Module Record,
        if (is<WebAssemblyModule>(*resolved_module)) {
            auto& resolved_webassembly_module = as<WebAssemblyModule>(*resolved_module);

            // 1. If resolvedModule.[[Instance]] is ~empty~, throw a {LinkError} exception.
            if (!resolved_webassembly_module.m_instance) {
                return vm.throw_completion<LinkError>("Module has not been instantiated"sv);
            }

            // 2. Assert: resolvedModule.[[Instance]] is a WebAssembly Instance object.
            // 3. Assert: resolvedModule.[[ModuleSource]] is a WebAssembly Module object.
            // 4. Let module be resolvedModule.[[ModuleSource]].[[Module]].
            auto module = resolved_webassembly_module.m_module_source->compiled_module();

            // 5. Let externval be instance_export(resolvedModule.[[Instance]], resolvedName).
            // https://webassembly.github.io/spec/core/appendix/embedding.html#embed-instance-export
            auto externval = resolved_webassembly_module.m_instance->module_instance()->exports().first_matching([resolved_name](auto const& export_instance) { return export_instance.name() == resolved_name; });

            // 6. Assert: externval is not error.
            VERIFY(externval.has_value());

            // 7. Assert: module_exports(module) contains an element (resolvedName, type).
            auto module_export = module->module->export_section().entries().first_matching([resolved_name](auto& element) { return element.name() == resolved_name; });
            VERIFY(module_export.has_value());

            // 8. Let externtype be the value of type for the element (resolvedName, type) in module_exports(module).
            auto externtype = module_export->description();

            // 9. If importtype is not an extern subtype of externtype, throw a LinkError exception.
            // https://webassembly.github.io/spec/core/valid/types.html#match-externtype
            auto& store = cache.abstract_machine().store();
            auto invalid = entry.description().visit(
                [&](Wasm::MemoryType const& mem_type) -> Optional<ByteString> {
                    if (!externtype.has<Wasm::MemoryIndex>())
                        return "Expected memory import"sv;
                    auto other_mem_type = store.get(Wasm::MemoryAddress { externtype.get<Wasm::MemoryIndex>().value() })->type();
                    if (other_mem_type.limits().is_subset_of(mem_type.limits()))
                        return {};
                    return ByteString::formatted("Memory import and extern do not match: {}-{} vs {}-{}", mem_type.limits().min(), mem_type.limits().max(), other_mem_type.limits().min(), other_mem_type.limits().max());
                },
                [&](Wasm::TableType const& table_type) -> Optional<ByteString> {
                    if (!externtype.has<Wasm::TableIndex>())
                        return "Expected table import"sv;
                    auto other_table_type = store.get(Wasm::TableAddress { externtype.get<Wasm::TableIndex>().value() })->type();
                    if (table_type.element_type() == other_table_type.element_type()
                        && other_table_type.limits().is_subset_of(table_type.limits()))
                        return {};

                    return ByteString::formatted("Table import and extern do not match: {}-{} vs {}-{}", table_type.limits().min(), table_type.limits().max(), other_table_type.limits().min(), other_table_type.limits().max());
                },
                [&](Wasm::GlobalType const& global_type) -> Optional<ByteString> {
                    if (!externtype.has<Wasm::GlobalIndex>())
                        return "Expected global import"sv;
                    auto other_global_type = store.get(Wasm::GlobalAddress { externtype.get<Wasm::GlobalIndex>().value() })->type();
                    if (global_type.type() == other_global_type.type()
                        && global_type.is_mutable() == other_global_type.is_mutable())
                        return {};
                    return "Global import and extern do not match"sv;
                },
                [&](Wasm::FunctionType const& type) -> Optional<ByteString> {
                    if (!externtype.has<Wasm::FunctionIndex>())
                        return "Expected function import"sv;
                    auto other_type = store.get(Wasm::FunctionAddress { externtype.get<Wasm::FunctionIndex>().value() })->visit([&](Wasm::WasmFunction const& wasm_func) { return wasm_func.type(); }, [&](Wasm::HostFunction const& host_func) { return host_func.type(); });
                    if (type.results() != other_type.results())
                        return ByteString::formatted("Function import and extern do not match, results: {} vs {}", type.results(), other_type.results());
                    if (type.parameters() != other_type.parameters())
                        return ByteString::formatted("Function import and extern do not match, parameters: {} vs {}", type.parameters(), other_type.parameters());
                    return {};
                },
                [&](Wasm::TagType const& type) -> Optional<ByteString> {
                    if (!externtype.has<Wasm::TagIndex>())
                        return "Expected tag import"sv;
                    auto* other_tag_instance = store.get(Wasm::TagAddress { externtype.get<Wasm::TagIndex>().value() });
                    if (other_tag_instance->flags() != type.flags())
                        return "Tag import and extern do not match"sv;

                    auto const& this_type = module->module->type_section().types()[type.type().value()];

                    if (other_tag_instance->type().parameters() != this_type.parameters())
                        return "Tag import and extern do not match"sv;
                    return {};
                },
                [&](Wasm::TypeIndex type_index) -> Optional<ByteString> {
                    if (!externtype.has<Wasm::FunctionIndex>())
                        return "Expected function import"sv;
                    auto other_type = store.get(Wasm::FunctionAddress { externtype.get<Wasm::FunctionIndex>().value() })->visit([&](Wasm::WasmFunction const& wasm_func) { return wasm_func.type(); }, [&](Wasm::HostFunction const& host_func) { return host_func.type(); });
                    auto const& type = module->module->type_section().types()[type_index.value()];
                    if (type.results() != other_type.results())
                        return ByteString::formatted("Function import and extern do not match, results: {} vs {}", type.results(), other_type.results());
                    if (type.parameters() != other_type.parameters())
                        return ByteString::formatted("Function import and extern do not match, parameters: {} vs {}", type.parameters(), other_type.parameters());
                    return {};
                });
            if (invalid.has_value())
                return vm.throw_completion<LinkError>(ByteString::formatted("{}::{}: {}", entry.module(), entry.name(), invalid.release_value()));

            // 10. Append externval to imports.
            imports.append(externval.value().value());
        }

        // 8. Otherwise,
        else {
            // 1. Let env be resolvedModule.[[Environment]].
            auto env = resolved_module->environment();

            // 2. Let v be ?env.GetBindingValue(resolvedName, true).
            auto v = TRY(env->get_binding_value(vm, resolved_name, true));

            // 3. If importtype is of the form func functype,
            // AD-HOC: Resolve type index
            if (entry.description().has<Wasm::FunctionType>() || entry.description().has<Wasm::TypeIndex>()) {
                auto functype = entry.description().visit(
                    [](Wasm::FunctionType function_type) { return function_type; },
                    [&module](Wasm::TypeIndex type_index) { return module->module->type_section().types()[type_index.value()]; },
                    [](auto) -> Wasm::FunctionType { VERIFY_NOT_REACHED(); });

                // 1. If IsCallable(v) is false, throw a LinkError exception.
                if (!v.is_function())
                    return vm.throw_completion<LinkError>(JS::ErrorType::NotAFunction, v);
                auto& function = v.as_function();

                // 2. If v has a [[FunctionAddress]] internal slot, and therefore is an Exported Function,
                Optional<Wasm::FunctionAddress> funcaddr;
                if (is<Detail::ExportedWasmFunction>(function)) {
                    // 1. Let funcaddr be the value of v’s [[FunctionAddress]] internal slot.
                    auto& exported_function = static_cast<Detail::ExportedWasmFunction&>(function);
                    funcaddr = exported_function.exported_address();
                }

                // 3. Otherwise,
                else {
                    // 1. Create a host function from v and functype, and let funcaddr be the result.
                    cache.add_imported_object(function);
                    auto host_function = Detail::create_host_function(vm, function, functype, ByteString::formatted("func{}", imports.size()));
                    funcaddr = cache.abstract_machine().store().allocate(move(host_function));

                    // FIXME: 2. Let index be the number of external functions in imports, defining the index of the host function funcaddr.
                }

                // 4. Let externfunc be the external value func funcaddr.
                Wasm::ExternValue externfunc { Wasm::FunctionAddress { *funcaddr } };

                // 5. Append externfunc to imports.
                imports.append(externfunc);
            }

            // 4. If importtype is of the form global mut valtype,
            if (entry.description().has<Wasm::GlobalType>()) {
                auto valtype = entry.description().get<Wasm::GlobalType>();

                // 1. Let store be the surrounding agent’s associated store.
                auto& store = cache.abstract_machine().store();

                // 2. If v implements Global,
                Optional<Wasm::GlobalAddress> globaladdr;
                if (v.is_object() && is<Global>(v.as_object())) {
                    // 1. Let globaladdr be v.[[Global]].
                    globaladdr = as<Global>(v.as_object()).address();

                    // 2. Let targetmut valuetype be global_type(store, globaladdr).
                    auto* valuetype = store.get(*globaladdr);

                    // 3. If mut is const and targetmut is var, throw a LinkError exception.
                    if (!valtype.is_mutable() && valuetype->is_mutable()) {
                        return vm.throw_completion<LinkError>("Mutable globals are not supported for immutable imports"sv);
                    }
                }

                // 3. Otherwise,
                else {
                    // AD-HOC: If valtype is i64 and v is a Number, throw a LinkError exception.
                    if (valtype.type().kind() == Wasm::ValueType::I64 && v.is_number()) {
                        return vm.throw_completion<LinkError>("Import resolution attempted to cast a Number to a BigInteger"sv);
                    }

                    // AD-HOC: If valtype is not i64 and v is a BigInt, throw a LinkError exception.
                    if (valtype.type().kind() != Wasm::ValueType::I64 && v.is_bigint()) {
                        return vm.throw_completion<LinkError>("Import resolution attempted to cast a BigInteger to a Number"sv);
                    }

                    // 1. If valtype is v128, throw a LinkError exception.
                    if (valtype.type().kind() == Wasm::ValueType::V128) {
                        return vm.throw_completion<LinkError>("V128 is not supported as a global value type"sv);
                    }

                    // 2. If mut is var, throw a LinkError exception.
                    if (valtype.is_mutable()) {
                        return vm.throw_completion<LinkError>("Variable global value types are not supported"sv);
                    }

                    // 3. Let value be ?ToWebAssemblyValue(v, valtype).
                    auto value = TRY(Detail::to_webassembly_value(vm, v, valtype.type()));

                    // 4. Let(store, globaladdr) be global_alloc(store, mut valtype, value).
                    // 5. Set the surrounding agent’s associated store to store.
                    globaladdr = cache.abstract_machine().store().allocate(valtype, value);
                }

                // 4. Let externglobal be global globaladdr.
                Wasm::ExternValue externglobal { Wasm::GlobalAddress { *globaladdr } };

                // 5. Append externglobal to imports.
                imports.append(externglobal);
            }

            // 5. If importtype is of the form mem memtype,
            if (entry.description().has<Wasm::MemoryType>()) {
                // 1. If v does not implement Memory, throw a LinkError exception.
                if (!v.is_object() || !is<WebAssembly::Memory>(v.as_object())) {
                    return vm.throw_completion<LinkError>("Expected an instance of WebAssembly.Memory for a memory import"sv);
                }

                // 2. Let externmem be the external value mem v.[[Memory]].
                auto externmem = static_cast<WebAssembly::Memory const&>(v.as_object()).address();

                // 3. Append externmem to imports.
                imports.append(externmem);
            }

            // 6. If importtype is of the form table tabletype,
            if (entry.description().has<Wasm::TableType>()) {
                // 1. If v does not implement Table, throw a LinkError exception.
                if (!v.is_object() || !is<WebAssembly::Table>(v.as_object())) {
                    return vm.throw_completion<LinkError>("Expected an instance of WebAssembly.Table for a table import"sv);
                }

                // 2. Let tableaddr be v.[[Table]].
                auto tableaddr = static_cast<WebAssembly::Table const&>(v.as_object()).address();

                // 3. Let externtable be the external value table tableaddr.
                Wasm::ExternValue externtable { tableaddr };

                // 4. Append externtable to imports.
                imports.append(externtable);
            }
        }
    }

    // 6. Instantiate the core of a WebAssembly module module with imports, and let instance be the result.
    // https://webassembly.github.io/spec/js-api/index.html#instantiate-the-core-of-a-webassembly-module
    auto instantiation_result = cache.abstract_machine().instantiate(module->module, imports);
    if (instantiation_result.is_error()) {
        auto instantiation_error = instantiation_result.release_error();
        switch (instantiation_error.source) {
        case Wasm::InstantiationErrorSource::Linking:
            return vm.throw_completion<LinkError>(instantiation_error.error);
        case Wasm::InstantiationErrorSource::StartFunction:
            return vm.throw_completion<RuntimeError>(instantiation_error.error);
        }
        VERIFY_NOT_REACHED();
    }

    // 7. Set record.[[Instance]] to instance.
    record->m_instance = vm.heap().allocate<Instance>(*vm.current_realm(), instantiation_result.release_value());

    // 8. For each (name, externtype) of module_exports(module),
    for (auto const& entry : module->module->export_section().entries()) {
        // 1. If externtype is of the form global mut globaltype,
        if (entry.description().has<Wasm::GlobalIndex>()) {
            // 1. Assert: externval is of the form global globaladdr.
            // 2. Let global globaladdr be externval.
            // 3. Let global_value be global_read(store, globaladdr).
            auto globaladdr = Wasm::GlobalAddress { entry.description().get<Wasm::GlobalIndex>().value() };
            auto* global_value = cache.abstract_machine().store().get(globaladdr);
            VERIFY(global_value);

            // 4. If globaltype is not v128,
            auto type = global_value->type();
            if (type.type().kind() != Wasm::ValueType::Kind::V128) {
                // NOTE: The condition above leaves unsupported JS values as uninitialized in TDZ and therefore as a
                //       reference error on access. When integrating with shared globals, they may be excluded here
                //       similarly to v128 above.

                // 1. Perform !record.[[Environment]].InitializeBinding(name, ToJSValue(global_value)).
                auto value = global_value->value();
                MUST(record->environment()->initialize_binding(vm, Utf16FlyString::from_utf8(entry.name()), Detail::to_js_value(vm, value, type.type()), JS::Environment::InitializeBindingHint::Normal));

                // FIXME: 2. If mut is var, then associate all future mutations of globaladdr with the ECMA-262 binding record
                //        for name in record.[[Environment]], such that record.[[Environment]].GetBindingValue(resolution.[[BindingName]], true)
                //        always returns ToJSValue(global_read(store, globaladdr)) for the current surrounding agent’s associated store store.
            }
        }

        // 2. Otherwise,
        else {
            // 1. Perform !record.[[Environment]].InitializeBinding(name, !Get(instance.[[Exports]], name)).
            auto name = Utf16FlyString::from_utf8(entry.name());
            MUST(record->environment()->initialize_binding(vm, name, MUST(record->m_instance->get(JS::PropertyKey { name })), JS::Environment::InitializeBindingHint::Normal));
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
