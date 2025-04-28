/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/SyntheticModule.h>

namespace JS {

GC_DEFINE_ALLOCATOR(SyntheticModule);

SyntheticModule::SyntheticModule(Realm& realm, Vector<FlyString> export_names, SyntheticModule::EvaluationFunction evaluation_steps, ByteString filename)
    : Module(realm, move(filename))
    , m_export_names(move(export_names))
    , m_evaluation_steps(evaluation_steps)
{
}

void SyntheticModule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_evaluation_steps);
}

// 16.2.1.8.1 CreateDefaultExportSyntheticModule ( defaultExport ), https://tc39.es/ecma262/#sec-create-default-export-synthetic-module
GC::Ref<SyntheticModule> SyntheticModule::create_default_export_synthetic_module(Realm& realm, Value default_export, ByteString filename)
{
    // 1. Let realm be the current Realm Record.

    // 2. Let setDefaultExport be a new Abstract Closure with parameters (module) that captures defaultExport and
    //    performs the following steps when called:
    auto set_default_export = GC::create_function(realm.heap(), [default_export](SyntheticModule& module) -> ThrowCompletionOr<void> {
        // a. Perform SetSyntheticModuleExport(module, "default", defaultExport).
        TRY(module.set_synthetic_module_export("default"_fly_string, default_export));

        // b. Return NormalCompletion(UNUSED).
        return {};
    });

    // 2. Return the Synthetic Module Record { [[Realm]]: realm, [[Environment]]: empty, [[Namespace]]: empty, [[HostDefined]]: undefined, [[ExportNames]]: « "default" », [[EvaluationSteps]]: setDefaultExport }.
    return realm.heap().allocate<SyntheticModule>(realm, Vector<FlyString> { "default"_fly_string }, set_default_export, move(filename));
}

// 16.2.1.8.2 ParseJSONModule ( source ), https://tc39.es/ecma262/#sec-create-default-export-synthetic-module
ThrowCompletionOr<GC::Ref<Module>> parse_json_module(Realm& realm, StringView source_text, ByteString filename)
{
    auto& vm = realm.vm();

    // 1. Let json be ? ParseJSON(source).
    auto json = TRY(JSONObject::parse_json(vm, source_text));

    // 3. Return CreateDefaultExportSyntheticModule(json).
    return SyntheticModule::create_default_export_synthetic_module(realm, json, move(filename));
}

// 16.2.1.8.3 SetSyntheticModuleExport ( module, exportName, exportValue ), https://tc39.es/ecma262/#sec-setsyntheticmoduleexport
ThrowCompletionOr<void> SyntheticModule::set_synthetic_module_export(FlyString const& export_name, Value export_value)
{
    auto& vm = this->vm();

    // 1. Assert: module.[[ExportNames]] contains exportName.
    VERIFY(m_export_names.contains_slow(export_name));

    // 2. Let envRec be module.[[Environment]].
    auto environment_record = environment();

    // 3. Assert: envRec is not EMPTY.
    VERIFY(environment_record);

    // 4. Perform envRec.SetMutableBinding(exportName, exportValue, true).
    TRY(environment_record->set_mutable_binding(vm, export_name, export_value, true));

    // 5. Return UNUSED.
    return {};
}

// 16.2.1.8.4.1 LoadRequestedModules ( ), https://tc39.es/ecma262/#sec-smr-LoadRequestedModules
PromiseCapability& SyntheticModule::load_requested_modules(GC::Ptr<GraphLoadingState::HostDefined>)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Return ! PromiseResolve(%Promise%, undefined).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));
    (void)MUST(call(vm, *promise_capability->resolve(), js_undefined(), js_undefined()));

    // NOTE: We need to return a PromiseCapability, rather than a Promise, so we flatten PromiseResolve here.
    //       This is likely a spec bug, see https://matrixlogs.bakkot.com/WHATWG/2023-02-13#L1
    return promise_capability;
}

// 16.2.1.8.4.2 GetExportedNames ( ), https://tc39.es/ecma262/#sec-smr-getexportednames
Vector<FlyString> SyntheticModule::get_exported_names(VM&, HashTable<Module const*>&)
{
    // 1. Return module.[[ExportNames]].
    return m_export_names;
}

// 16.2.1.8.4.3 ResolveExport ( exportName ), https://tc39.es/ecma262/#sec-smr-resolveexport
ResolvedBinding SyntheticModule::resolve_export(VM&, FlyString const& export_name, Vector<ResolvedBinding>)
{
    // 1. If module.[[ExportNames]] does not contain exportName, return null.
    if (!m_export_names.contains_slow(export_name))
        return ResolvedBinding::null();

    // 2. Return ResolvedBinding Record { [[Module]]: module, [[BindingName]]: exportName }.
    return ResolvedBinding { ResolvedBinding::BindingName, this, export_name };
}

// 16.2.1.8.4.4 Link ( ), https://tc39.es/ecma262/#sec-smr-Link
ThrowCompletionOr<void> SyntheticModule::link(VM& vm)
{
    // 1. Let realm be module.[[Realm]].
    auto& realm = this->realm();

    // 2. Let env be NewModuleEnvironment(realm.[[GlobalEnv]]).
    auto environment = vm.heap().allocate<ModuleEnvironment>(&realm.global_environment());

    // 3. Set module.[[Environment]] to env.
    set_environment(environment);

    // 4. For each String exportName of module.[[ExportNames]], do
    for (auto const& export_name : m_export_names) {
        // a. Perform ! env.CreateMutableBinding(exportName, false).
        MUST(environment->create_mutable_binding(vm, export_name, false));

        // b. Perform ! env.InitializeBinding(exportName, undefined).
        MUST(environment->initialize_binding(vm, export_name, js_undefined(), Environment::InitializeBindingHint::Normal));
    }

    // 5. Return NormalCompletion(unused).
    return {};
}

// 16.2.1.8.4.5 Evaluate ( ), https://tc39.es/ecma262/#sec-smr-Evaluate
ThrowCompletionOr<GC::Ref<Promise>> SyntheticModule::evaluate(VM& vm)
{
    auto& realm = this->realm();

    // 1. Let moduleContext be a new ECMAScript code execution context.
    // 2. Set the Function of moduleContext to null.
    ExecutionContext* module_context = nullptr;
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(module_context, 0, 0);

    // 3. Set the Realm of moduleContext to module.[[Realm]].
    module_context->realm = &realm;

    // 4. Set the ScriptOrModule of moduleContext to module.
    module_context->script_or_module = GC::Ref<Module>(*this);

    // 5. Set the VariableEnvironment of moduleContext to module.[[Environment]].
    module_context->variable_environment = environment();

    // 6. Set the LexicalEnvironment of moduleContext to module.[[Environment]].
    module_context->lexical_environment = environment();

    // 7. Suspend the running execution context.
    // 8. Push moduleContext onto the execution context stack; moduleContext is now the running execution context.
    TRY(vm.push_execution_context(*module_context, {}));

    // 9. Let steps be module.[[EvaluationSteps]].
    // 10. Let result be Completion(steps(module)).
    auto result = m_evaluation_steps->function()(*this);

    // 11. Suspend moduleContext and remove it from the execution context stack.
    // 12. Resume the context that is now on the top of the execution context stack as the running execution context.
    vm.pop_execution_context();

    // 13. Let pc be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 14. IfAbruptRejectPromise(result, pc).
    if (result.is_error())
        MUST(call(vm, *promise_capability->reject(), JS::js_undefined(), result.release_error().value()));

    // 15. Perform ! Call(pc.[[Resolve]], undefined, « undefined »).
    else
        MUST(call(vm, *promise_capability->resolve(), js_undefined(), js_undefined()));

    // 16. Return pc.[[Promise]].
    return static_cast<Promise&>(*promise_capability->promise());
}

}
