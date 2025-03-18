/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/CyclicModule.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/ModuleNamespaceObject.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Module);
GC_DEFINE_ALLOCATOR(GraphLoadingState);

Module::Module(Realm& realm, ByteString filename, Script::HostDefined* host_defined)
    : m_realm(realm)
    , m_host_defined(host_defined)
    , m_filename(move(filename))
{
}

Module::~Module() = default;

void Module::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_environment);
    visitor.visit(m_namespace);
    if (m_host_defined)
        m_host_defined->visit_host_defined_self(visitor);
}

// 16.2.1.5.1.1 InnerModuleLinking ( module, stack, index ), https://tc39.es/ecma262/#sec-InnerModuleLinking
ThrowCompletionOr<u32> Module::inner_module_linking(VM& vm, Vector<Module*>&, u32 index)
{
    // 1. If module is not a Cyclic Module Record, then
    // a. Perform ? module.Link().
    TRY(link(vm));
    // b. Return index.
    return index;
}

// 16.2.1.5.2.1 InnerModuleEvaluation ( module, stack, index ), https://tc39.es/ecma262/#sec-innermoduleevaluation
ThrowCompletionOr<u32> Module::inner_module_evaluation(VM& vm, Vector<Module*>&, u32 index)
{
    // 1. If module is not a Cyclic Module Record, then
    // a. Let promise be ! module.Evaluate().
    auto promise = TRY(evaluate(vm));

    // b. Assert: promise.[[PromiseState]] is not pending.
    VERIFY(promise->state() != Promise::State::Pending);

    // c. If promise.[[PromiseState]] is rejected, then
    if (promise->state() == Promise::State::Rejected) {
        // i. Return ThrowCompletion(promise.[[PromiseResult]]).
        return throw_completion(promise->result());
    }

    // d. Return index.
    return index;
}

// 16.2.1.9 FinishLoadingImportedModule ( referrer, specifier, payload, result ), https://tc39.es/ecma262/#sec-FinishLoadingImportedModule
void finish_loading_imported_module(ImportedModuleReferrer referrer, ModuleRequest const& module_request, ImportedModulePayload payload, ThrowCompletionOr<GC::Ref<Module>> const& result)
{
    // 1. If result is a normal completion, then
    if (!result.is_error()) {
        // NOTE: Only Script and CyclicModule referrers have the [[LoadedModules]] internal slot.
        if (referrer.has<GC::Ref<Script>>() || referrer.has<GC::Ref<CyclicModule>>()) {
            auto& loaded_modules = referrer.visit(
                [](GC::Ref<JS::Realm>&) -> Vector<ModuleWithSpecifier>& {
                    VERIFY_NOT_REACHED();
                    __builtin_unreachable();
                },
                [](auto& script_or_module) -> Vector<ModuleWithSpecifier>& {
                    return script_or_module->loaded_modules();
                });

            bool found_record = false;

            // a. If referrer.[[LoadedModules]] contains a Record whose [[Specifier]] is specifier, then
            for (auto const& record : loaded_modules) {
                if (record.specifier == module_request.module_specifier) {
                    // i. Assert: That Record's [[Module]] is result.[[Value]].
                    VERIFY(record.module == result.value());
                    found_record = true;
                }
            }

            // b. Else,
            if (!found_record) {
                auto module = result.value();

                // i. Append the Record { [[Specifier]]: specifier, [[Module]]: result.[[Value]] } to referrer.[[LoadedModules]].
                loaded_modules.append(ModuleWithSpecifier {
                    .specifier = module_request.module_specifier.to_string(),
                    .module = GC::Ref<Module>(*module) });
            }
        }
    }

    // 2. If payload is a GraphLoadingState Record, then
    if (payload.has<GC::Ref<GraphLoadingState>>()) {
        // a. Perform ContinueModuleLoading(payload, result)
        continue_module_loading(payload.get<GC::Ref<GraphLoadingState>>(), result);
    }
    // 3. Else,
    else {
        // a. Perform ContinueDynamicImport(payload, result).
        continue_dynamic_import(payload.get<GC::Ref<PromiseCapability>>(), result);
    }

    // 4. Return unused.
}

// 16.2.1.10 GetModuleNamespace ( module ), https://tc39.es/ecma262/#sec-getmodulenamespace
ThrowCompletionOr<Object*> Module::get_module_namespace(VM& vm)
{
    // 1. Assert: If module is a Cyclic Module Record, then module.[[Status]] is not unlinked.
    // FIXME: Spec bug: https://github.com/tc39/ecma262/issues/3114

    // 2. Let namespace be module.[[Namespace]].
    auto* namespace_ = m_namespace.ptr();

    // 3. If namespace is empty, then
    if (!namespace_) {
        // a. Let exportedNames be ? module.GetExportedNames().
        auto exported_names = TRY(get_exported_names(vm));

        // b. Let unambiguousNames be a new empty List.
        Vector<FlyString> unambiguous_names;

        // c. For each element name of exportedNames, do
        for (auto& name : exported_names) {
            // i. Let resolution be ? module.ResolveExport(name).
            auto resolution = TRY(resolve_export(vm, name));

            // ii. If resolution is a ResolvedBinding Record, append name to unambiguousNames.
            if (resolution.is_valid())
                unambiguous_names.append(name);
        }

        // d. Set namespace to ModuleNamespaceCreate(module, unambiguousNames).
        namespace_ = module_namespace_create(unambiguous_names);
        VERIFY(m_namespace);
        // Note: This set the local variable 'namespace' and not the member variable which is done by ModuleNamespaceCreate
    }

    // 4. Return namespace.
    return namespace_;
}

// 10.4.6.12 ModuleNamespaceCreate ( module, exports ), https://tc39.es/ecma262/#sec-modulenamespacecreate
Object* Module::module_namespace_create(Vector<FlyString> unambiguous_names)
{
    auto& realm = this->realm();

    // 1. Assert: module.[[Namespace]] is empty.
    VERIFY(!m_namespace);

    // 2. Let internalSlotsList be the internal slots listed in Table 34.
    // 3. Let M be MakeBasicObject(internalSlotsList).
    // 4. Set M's essential internal methods to the definitions specified in 10.4.6.
    // 5. Set M.[[Module]] to module.
    // 6. Let sortedExports be a List whose elements are the elements of exports ordered as if an Array of the same values had been sorted using %Array.prototype.sort% using undefined as comparefn.
    // 7. Set M.[[Exports]] to sortedExports.
    // 8. Create own properties of M corresponding to the definitions in 28.3.
    auto module_namespace = realm.create<ModuleNamespaceObject>(realm, this, move(unambiguous_names));

    // 9. Set module.[[Namespace]] to M.
    m_namespace = make_root(module_namespace);

    // 10. Return M.
    return module_namespace;
}

}
