/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/TypeCasts.h>
#include <LibJS/CyclicModule.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

GC_DEFINE_ALLOCATOR(CyclicModule);

CyclicModule::CyclicModule(Realm& realm, StringView filename, bool has_top_level_await, Vector<ModuleRequest> requested_modules, Script::HostDefined* host_defined)
    : Module(realm, filename, host_defined)
    , m_requested_modules(move(requested_modules))
    , m_has_top_level_await(has_top_level_await)
{
}

CyclicModule::~CyclicModule() = default;

void CyclicModule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cycle_root);
    visitor.visit(m_top_level_capability);
    visitor.visit(m_async_parent_modules);
    for (auto const& loaded_module : m_loaded_modules)
        visitor.visit(loaded_module.module);
}

void GraphLoadingState::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(promise_capability);
    visitor.visit(host_defined);
    visitor.visit(visited);
}

// 16.2.1.5.1 LoadRequestedModules ( [ hostDefined ] ), https://tc39.es/ecma262/#sec-LoadRequestedModules
PromiseCapability& CyclicModule::load_requested_modules(GC::Ptr<GraphLoadingState::HostDefined> host_defined)
{
    auto& vm = this->vm();

    // 1. If hostDefined is not present, let hostDefined be EMPTY.
    // NOTE: The empty state is handled by hostDefined being an optional without value.

    // 2. Let pc be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, vm.current_realm()->intrinsics().promise_constructor()));

    // 3. Let state be the GraphLoadingState Record { [[IsLoading]]: true, [[PendingModulesCount]]: 1, [[Visited]]: « », [[PromiseCapability]]: pc, [[HostDefined]]: hostDefined }.
    auto state = heap().allocate<GraphLoadingState>(promise_capability, true, 1, HashTable<GC::Ptr<CyclicModule>> {}, move(host_defined));

    // 4. Perform InnerModuleLoading(state, module).
    inner_module_loading(vm, state, *this);

    // NOTE: This is likely a spec bug, see https://matrixlogs.bakkot.com/WHATWG/2023-02-13#L1
    // FIXME: 5. Return pc.[[Promise]].
    return promise_capability;
}

// 16.2.1.5.1.1 InnerModuleLoading ( state, module ), https://tc39.es/ecma262/#sec-InnerModuleLoading
void inner_module_loading(VM& vm, JS::GraphLoadingState& state, GC::Ref<Module> module)
{
    // 1. Assert: state.[[IsLoading]] is true.
    VERIFY(state.is_loading);

    // 2. If module is a Cyclic Module Record, module.[[Status]] is NEW, and state.[[Visited]] does not contain module, then
    if (auto* cyclic_module = as_if<CyclicModule>(*module); cyclic_module && cyclic_module->status() == ModuleStatus::New && !state.visited.contains(cyclic_module)) {
        // a. Append module to state.[[Visited]].
        state.visited.set(cyclic_module);

        // b. Let requestedModulesCount be the number of elements in module.[[RequestedModules]].
        auto requested_modules_count = cyclic_module->requested_modules().size();

        // c. Set state.[[PendingModulesCount]] to state.[[PendingModulesCount]] + requestedModulesCount.
        state.pending_module_count += requested_modules_count;

        // d. For each String required of module.[[RequestedModules]], do
        for (auto const& required : cyclic_module->requested_modules()) {
            bool found_record_in_loaded_modules = false;

            // i. If module.[[LoadedModules]] contains a Record whose [[Specifier]] is required, then
            for (auto const& record : cyclic_module->loaded_modules()) {
                if (record.specifier == required.module_specifier) {
                    // 1. Let record be that Record.

                    // 2. Perform InnerModuleLoading(state, record.[[Module]]).
                    inner_module_loading(vm, state, record.module);

                    found_record_in_loaded_modules = true;
                    break;
                }
            }

            // ii. Else,
            if (!found_record_in_loaded_modules) {
                // 1. Perform HostLoadImportedModule(module, required, state.[[HostDefined]], state).
                vm.host_load_imported_module(GC::Ref { *cyclic_module }, required, state.host_defined, GC::Ref<GraphLoadingState> { state });

                // 2. NOTE: HostLoadImportedModule will call FinishLoadingImportedModule, which re-enters the graph loading process through ContinueModuleLoading.
            }

            // iii. If state.[[IsLoading]] is false, return UNUSED.
            if (!state.is_loading)
                return;
        }
    }

    // 3. Assert: state.[[PendingModulesCount]] ≥ 1.
    VERIFY(state.pending_module_count >= 1);

    // 4. Set state.[[PendingModulesCount]] to state.[[PendingModulesCount]] - 1.
    --state.pending_module_count;

    // 5. If state.[[PendingModulesCount]] = 0, then
    if (state.pending_module_count == 0) {
        // a. Set state.[[IsLoading]] to false.
        state.is_loading = false;

        // b. For each Cyclic Module Record loaded of state.[[Visited]], do
        for (auto const& loaded : state.visited) {
            // i. If loaded.[[Status]] is NEW, set loaded.[[Status]] to UNLINKED.
            if (loaded->status() == ModuleStatus::New)
                loaded->set_status(ModuleStatus::Unlinked);
        }

        // c. Perform ! Call(state.[[PromiseCapability]].[[Resolve]], undefined, « undefined »).
        MUST(call(vm, *state.promise_capability->resolve(), js_undefined(), js_undefined()));
    }

    // 6. Return unused.
}

// 16.2.1.5.1.2 ContinueModuleLoading ( state, moduleCompletion ), https://tc39.es/ecma262/#sec-ContinueModuleLoading
void continue_module_loading(GraphLoadingState& state, ThrowCompletionOr<GC::Ref<Module>> const& module_completion)
{
    // 1. If state.[[IsLoading]] is false, return UNUSED.
    if (!state.is_loading)
        return;

    // 2. If moduleCompletion is a normal completion, then
    if (!module_completion.is_error()) {
        auto module = module_completion.value();

        // a. Perform InnerModuleLoading(state, moduleCompletion.[[Value]]).
        inner_module_loading(state.vm(), state, module);
    }
    // 3. Else,
    else {
        // a. Set state.[[IsLoading]] to false.
        state.is_loading = false;

        auto value = module_completion.throw_completion().value();

        // b. Perform ! Call(state.[[PromiseCapability]].[[Reject]], undefined, « moduleCompletion.[[Value]] »).
        MUST(call(state.vm(), *state.promise_capability->reject(), js_undefined(), *value));
    }

    // 4. Return UNUSED.
}

// 16.2.1.5.2 Link ( ), https://tc39.es/ecma262/#sec-moduledeclarationlinking
ThrowCompletionOr<void> CyclicModule::link(VM& vm)
{
    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] link[{}]()", this);
    // 1. Assert: module.[[Status]] is one of unlinked, linked, evaluating-async, or evaluated.
    VERIFY(m_status == ModuleStatus::Unlinked || m_status == ModuleStatus::Linked || m_status == ModuleStatus::EvaluatingAsync || m_status == ModuleStatus::Evaluated);
    // 2. Let stack be a new empty List.
    Vector<Module*> stack;

    // 3. Let result be Completion(InnerModuleLinking(module, stack, 0)).
    auto result = inner_module_linking(vm, stack, 0);

    // 4. If result is an abrupt completion, then
    if (result.is_throw_completion()) {
        // a. For each Cyclic Module Record m of stack, do
        for (auto* module : stack) {
            if (is<CyclicModule>(module)) {
                auto& cyclic_module = static_cast<CyclicModule&>(*module);
                // i. Assert: m.[[Status]] is linking.
                VERIFY(cyclic_module.m_status == ModuleStatus::Linking);

                // ii. Set m.[[Status]] to unlinked.
                cyclic_module.m_status = ModuleStatus::Unlinked;
            }
        }
        // b. Assert: module.[[Status]] is unlinked.
        VERIFY(m_status == ModuleStatus::Unlinked);

        // c. Return ? result.
        return result.release_error();
    }

    // 5. Assert: module.[[Status]] is one of linked, evaluating-async, or evaluated.
    VERIFY(m_status == ModuleStatus::Linked || m_status == ModuleStatus::EvaluatingAsync || m_status == ModuleStatus::Evaluated);
    // 6. Assert: stack is empty.
    VERIFY(stack.is_empty());

    // 7. Return unused.
    return {};
}

// 16.2.1.5.1.1 InnerModuleLinking ( module, stack, index ), https://tc39.es/ecma262/#sec-InnerModuleLinking
ThrowCompletionOr<u32> CyclicModule::inner_module_linking(VM& vm, Vector<Module*>& stack, u32 index)
{
    // 1. If module is not a Cyclic Module Record, then
    //    a. Perform ? module.Link().
    //    b. Return index.
    // Note: Step 1, 1.a and 1.b are handled in Module.cpp

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] inner_module_linking[{}](vm, {}, {})", this, ByteString::join(',', stack), index);

    // 2. If module.[[Status]] is linking, linked, evaluating-async, or evaluated, then
    if (m_status == ModuleStatus::Linking || m_status == ModuleStatus::Linked || m_status == ModuleStatus::EvaluatingAsync || m_status == ModuleStatus::Evaluated) {
        // a. Return index.
        return index;
    }

    // 3. Assert: module.[[Status]] is unlinked.
    VERIFY(m_status == ModuleStatus::Unlinked);

    // 4. Set module.[[Status]] to linking.
    m_status = ModuleStatus::Linking;

    // 5. Set module.[[DFSIndex]] to index.
    m_dfs_index = index;

    // 6. Set module.[[DFSAncestorIndex]] to index.
    m_dfs_ancestor_index = index;

    // 7. Set index to index + 1.
    ++index;

    // 8. Append module to stack.
    stack.append(this);

#if JS_MODULE_DEBUG
    StringBuilder request_module_names;
    for (auto& module_request : m_requested_modules) {
        request_module_names.append(module_request.module_specifier);
        request_module_names.append(", "sv);
    }
    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] module: {} has requested modules: [{}]", filename(), request_module_names.string_view());
#endif

    // 9. For each String required of module.[[RequestedModules]], do
    for (auto& required_string : m_requested_modules) {
        ModuleRequest required { required_string };

        // a. Let requiredModule be GetImportedModule(module, required).
        auto required_module = get_imported_module(required);

        // b. Set index to ? InnerModuleLinking(requiredModule, stack, index).
        index = TRY(required_module->inner_module_linking(vm, stack, index));

        // c. If requiredModule is a Cyclic Module Record, then
        if (is<CyclicModule>(*required_module)) {
            auto& cyclic_module = static_cast<CyclicModule&>(*required_module);
            // i. Assert: requiredModule.[[Status]] is either linking, linked, evaluating-async, or evaluated.
            VERIFY(cyclic_module.m_status == ModuleStatus::Linking || cyclic_module.m_status == ModuleStatus::Linked || cyclic_module.m_status == ModuleStatus::EvaluatingAsync || cyclic_module.m_status == ModuleStatus::Evaluated);

            // ii. Assert: requiredModule.[[Status]] is linking if and only if requiredModule is in stack.
            VERIFY((cyclic_module.m_status == ModuleStatus::Linking) == (stack.contains_slow(&cyclic_module)));

            // iii. If requiredModule.[[Status]] is linking, then
            if (cyclic_module.m_status == ModuleStatus::Linking) {
                // 1. Set module.[[DFSAncestorIndex]] to min(module.[[DFSAncestorIndex]], requiredModule.[[DFSAncestorIndex]]).
                m_dfs_ancestor_index = min(m_dfs_ancestor_index.value(), cyclic_module.m_dfs_ancestor_index.value());
            }
        }
    }

    // 10. Perform ? module.InitializeEnvironment().
    TRY(initialize_environment(vm));

    // 11. Assert: module occurs exactly once in stack.
    size_t count = 0;
    for (auto* module : stack) {
        if (module == this)
            count++;
    }
    VERIFY(count == 1);

    // 12. Assert: module.[[DFSAncestorIndex]] ≤ module.[[DFSIndex]].
    VERIFY(m_dfs_ancestor_index.value() <= m_dfs_index.value());

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] module {} after inner_linking has dfs {} and ancestor dfs {}", filename(), m_dfs_index.value(), m_dfs_ancestor_index.value());

    // 13. If module.[[DFSAncestorIndex]] = module.[[DFSIndex]], then
    if (m_dfs_ancestor_index == m_dfs_index) {
        // a. Let done be false.
        // b. Repeat, while done is false,
        while (true) {
            // i. Let requiredModule be the last element in stack.
            // ii. Remove the last element of stack.
            auto* required_module = stack.take_last();

            // iii. Assert: requiredModule is a Cyclic Module Record.
            VERIFY(is<CyclicModule>(*required_module));

            // iv. Set requiredModule.[[Status]] to linked.
            static_cast<CyclicModule&>(*required_module).m_status = ModuleStatus::Linked;

            // v. If requiredModule and module are the same Module Record, set done to true.
            if (required_module == this)
                break;
        }
    }

    // 14. Return index.
    return index;
}

// 16.2.1.5.3 Evaluate ( ), https://tc39.es/ecma262/#sec-moduleevaluation
ThrowCompletionOr<Promise*> CyclicModule::evaluate(VM& vm)
{
    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] evaluate[{}](vm)", this);
    // 1. Assert: This call to Evaluate is not happening at the same time as another call to Evaluate within the surrounding agent.
    // FIXME: Verify this somehow

    // 2. Assert: module.[[Status]] is one of linked, evaluating-async, or evaluated.
    VERIFY(m_status == ModuleStatus::Linked || m_status == ModuleStatus::EvaluatingAsync || m_status == ModuleStatus::Evaluated);

    // NOTE: The spec does not catch the case where evaluate is called twice on a script which failed
    //       during evaluation. This means the script is evaluated but does not have a cycle root.
    //       In that case we first check if this module itself has a top level capability.
    //       See also: https://github.com/tc39/ecma262/issues/2823 .
    if (m_top_level_capability != nullptr)
        return as<Promise>(m_top_level_capability->promise().ptr());

    // 3. If module.[[Status]] is either evaluating-async or evaluated, set module to module.[[CycleRoot]].
    if ((m_status == ModuleStatus::EvaluatingAsync || m_status == ModuleStatus::Evaluated) && m_cycle_root != this) {
        // Note: This will continue this function with module.[[CycleRoot]]
        VERIFY(m_cycle_root);
        VERIFY(m_cycle_root->m_status == ModuleStatus::Linked);
        dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] evaluate[{}](vm) deferring to cycle root at {}", this, m_cycle_root.ptr());
        return m_cycle_root->evaluate(vm);
    }

    // 4. If module.[[TopLevelCapability]] is not empty, then
    if (m_top_level_capability != nullptr) {
        // a. Return module.[[TopLevelCapability]].[[Promise]].
        return as<Promise>(m_top_level_capability->promise().ptr());
    }

    // 5. Let stack be a new empty List.
    Vector<Module*> stack;

    auto& realm = *vm.current_realm();

    // 6. Let capability be ! NewPromiseCapability(%Promise%).
    // 7. Set module.[[TopLevelCapability]] to capability.
    m_top_level_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 8. Let result be Completion(InnerModuleEvaluation(module, stack, 0)).
    auto result = inner_module_evaluation(vm, stack, 0);

    // 9. If result is an abrupt completion, then
    if (result.is_throw_completion()) {
        VERIFY(!m_evaluation_error.is_error());

        // a. For each Cyclic Module Record m of stack, do
        for (auto* mod : stack) {
            if (!is<CyclicModule>(*mod))
                continue;

            auto& cyclic_module = static_cast<CyclicModule&>(*mod);

            // i. Assert: m.[[Status]] is evaluating.
            VERIFY(cyclic_module.m_status == ModuleStatus::Evaluating);

            // ii. Set m.[[Status]] to evaluated.
            cyclic_module.m_status = ModuleStatus::Evaluated;

            // iii. Set m.[[EvaluationError]] to result.
            cyclic_module.m_evaluation_error = result.throw_completion();
        }

        // b. Assert: module.[[Status]] is evaluated.
        VERIFY(m_status == ModuleStatus::Evaluated);

        // c. Assert: module.[[EvaluationError]] is result.
        VERIFY(m_evaluation_error.is_error());
        VERIFY(same_value(*m_evaluation_error.throw_completion().value(), *result.throw_completion().value()));

        // d. Perform ! Call(capability.[[Reject]], undefined, « result.[[Value]] »).
        MUST(call(vm, *m_top_level_capability->reject(), js_undefined(), *result.throw_completion().value()));
    }
    // 10. Else,
    else {
        // a. Assert: module.[[Status]] is either evaluating-async or evaluated.
        VERIFY(m_status == ModuleStatus::EvaluatingAsync || m_status == ModuleStatus::Evaluated);
        // b. Assert: module.[[EvaluationError]] is empty.
        VERIFY(!m_evaluation_error.is_error());

        // c. If module.[[AsyncEvaluation]] is false, then
        if (!m_async_evaluation) {
            // i. Assert: module.[[Status]] is evaluated.
            VERIFY(m_status == ModuleStatus::Evaluated);
            // ii. Perform ! Call(capability.[[Resolve]], undefined, « undefined »).
            MUST(call(vm, *m_top_level_capability->resolve(), js_undefined(), js_undefined()));
        }

        // d. Assert: stack is empty.
        VERIFY(stack.is_empty());
    }

    // 11. Return capability.[[Promise]].
    return as<Promise>(m_top_level_capability->promise().ptr());
}

// 16.2.1.5.2.1 InnerModuleEvaluation ( module, stack, index ), https://tc39.es/ecma262/#sec-innermoduleevaluation
ThrowCompletionOr<u32> CyclicModule::inner_module_evaluation(VM& vm, Vector<Module*>& stack, u32 index)
{
    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] inner_module_evaluation[{}](vm, {}, {})", this, ByteString::join(", "sv, stack), index);
    // Note: Step 1 is performed in Module.cpp

    // 2. If module.[[Status]] is evaluating-async or evaluated, then
    if (m_status == ModuleStatus::EvaluatingAsync || m_status == ModuleStatus::Evaluated) {
        // a. If module.[[EvaluationError]] is empty, return index.
        if (!m_evaluation_error.is_error())
            return index;

        // b. Otherwise, return ? module.[[EvaluationError]].
        return m_evaluation_error.throw_completion();
    }

    // 3. If module.[[Status]] is evaluating, return index.
    if (m_status == ModuleStatus::Evaluating)
        return index;

    // 4. Assert: module.[[Status]] is linked.
    VERIFY(m_status == ModuleStatus::Linked);

    // 5. Set module.[[Status]] to evaluating.
    m_status = ModuleStatus::Evaluating;

    // 6. Set module.[[DFSIndex]] to index.
    m_dfs_index = index;

    // 7. Set module.[[DFSAncestorIndex]] to index.
    m_dfs_ancestor_index = index;

    // 8. Set module.[[PendingAsyncDependencies]] to 0.
    m_pending_async_dependencies = 0;

    // 9. Set index to index + 1.
    ++index;

    // 10. Append module to stack.
    stack.append(this);

    // 11. For each String required of module.[[RequestedModules]], do
    for (auto& required : m_requested_modules) {

        // a. Let requiredModule be GetImportedModule(module, required).
        auto required_module = get_imported_module(required);

        // b. Set index to ? InnerModuleEvaluation(requiredModule, stack, index).
        index = TRY(required_module->inner_module_evaluation(vm, stack, index));

        // c. If requiredModule is a Cyclic Module Record, then
        if (!is<CyclicModule>(*required_module))
            continue;

        GC::Ref<CyclicModule> cyclic_module = as<CyclicModule>(*required_module);
        // i. Assert: requiredModule.[[Status]] is either evaluating, evaluating-async, or evaluated.
        VERIFY(cyclic_module->m_status == ModuleStatus::Evaluating || cyclic_module->m_status == ModuleStatus::EvaluatingAsync || cyclic_module->m_status == ModuleStatus::Evaluated);

        // ii. Assert: requiredModule.[[Status]] is evaluating if and only if requiredModule is in stack.
        VERIFY(cyclic_module->m_status != ModuleStatus::Evaluating || stack.contains_slow(cyclic_module));

        // iii. If requiredModule.[[Status]] is evaluating, then
        if (cyclic_module->m_status == ModuleStatus::Evaluating) {
            // 1. Set module.[[DFSAncestorIndex]] to min(module.[[DFSAncestorIndex]], requiredModule.[[DFSAncestorIndex]]).
            m_dfs_ancestor_index = min(m_dfs_ancestor_index.value(), cyclic_module->m_dfs_ancestor_index.value());
        }
        // iv. Else,
        else {
            // 1. Set requiredModule to requiredModule.[[CycleRoot]].
            VERIFY(cyclic_module->m_cycle_root);
            cyclic_module = *cyclic_module->m_cycle_root;

            // 2. Assert: requiredModule.[[Status]] is evaluating-async or evaluated.
            VERIFY(cyclic_module->m_status == ModuleStatus::EvaluatingAsync || cyclic_module->m_status == ModuleStatus::Evaluated);

            // 3. If requiredModule.[[EvaluationError]] is not empty, return ? requiredModule.[[EvaluationError]].
            if (cyclic_module->m_evaluation_error.is_error())
                return cyclic_module->m_evaluation_error.throw_completion();
        }

        // v. If requiredModule.[[AsyncEvaluation]] is true, then
        if (cyclic_module->m_async_evaluation) {
            // 1. Set module.[[PendingAsyncDependencies]] to module.[[PendingAsyncDependencies]] + 1.
            ++m_pending_async_dependencies.value();

            // 2. Append module to requiredModule.[[AsyncParentModules]].
            cyclic_module->m_async_parent_modules.append(this);
        }
    }

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] inner_module_evaluation on {} has tla: {} and pending async dep: {} dfs: {} ancestor dfs: {}", filename(), m_has_top_level_await, m_pending_async_dependencies.value(), m_dfs_index.value(), m_dfs_ancestor_index.value());
    // 12. If module.[[PendingAsyncDependencies]] > 0 or module.[[HasTLA]] is true, then
    if (m_pending_async_dependencies.value() > 0 || m_has_top_level_await) {
        // a. Assert: module.[[AsyncEvaluation]] is false and was never previously set to true.
        VERIFY(!m_async_evaluation); // FIXME: I don't think we can check previously?

        // b. Set module.[[AsyncEvaluation]] to true.
        m_async_evaluation = true;
        // c. NOTE: The order in which module records have their [[AsyncEvaluation]] fields transition to true is significant. (See 16.2.1.5.2.4.)

        // d. If module.[[PendingAsyncDependencies]] is 0, perform ExecuteAsyncModule(module).
        if (m_pending_async_dependencies.value() == 0)
            execute_async_module(vm);
    }
    // 13. Otherwise, perform ? module.ExecuteModule().
    else {
        TRY(execute_module(vm));
    }

    // 14. Assert: module occurs exactly once in stack.
    auto count = 0;
    for (auto* module : stack) {
        if (module == this)
            count++;
    }
    VERIFY(count == 1);

    // 15. Assert: module.[[DFSAncestorIndex]] ≤ module.[[DFSIndex]].
    VERIFY(m_dfs_ancestor_index.value() <= m_dfs_index.value());

    // 16. If module.[[DFSAncestorIndex]] = module.[[DFSIndex]], then
    if (m_dfs_ancestor_index == m_dfs_index) {
        // a. Let done be false.
        bool done = false;
        // b. Repeat, while done is false,
        while (!done) {

            // i. Let requiredModule be the last element in stack.
            // ii. Remove the last element of stack.
            auto* required_module = stack.take_last();

            // iii. Assert: requiredModule is a Cyclic Module Record.
            VERIFY(is<CyclicModule>(*required_module));

            auto& cyclic_module = static_cast<CyclicModule&>(*required_module);

            // iv. If requiredModule.[[AsyncEvaluation]] is false, set requiredModule.[[Status]] to evaluated.
            if (!cyclic_module.m_async_evaluation)
                cyclic_module.m_status = ModuleStatus::Evaluated;
            // v. Otherwise, set requiredModule.[[Status]] to evaluating-async.
            else
                cyclic_module.m_status = ModuleStatus::EvaluatingAsync;

            // vi. If requiredModule and module are the same Module Record, set done to true.
            if (required_module == this)
                done = true;

            // vii. Set requiredModule.[[CycleRoot]] to module.
            cyclic_module.m_cycle_root = this;
        }
    }

    // 17. Return index.
    return index;
}

ThrowCompletionOr<void> CyclicModule::initialize_environment(VM&)
{
    // Note: In ecma262 this is never called on a cyclic module only on SourceTextModules.
    //       So this check is to make sure we don't accidentally call this.
    VERIFY_NOT_REACHED();
}

ThrowCompletionOr<void> CyclicModule::execute_module(VM&, GC::Ptr<PromiseCapability>)
{
    // Note: In ecma262 this is never called on a cyclic module only on SourceTextModules.
    //       So this check is to make sure we don't accidentally call this.
    VERIFY_NOT_REACHED();
}

// 16.2.1.5.2.2 ExecuteAsyncModule ( module ), https://tc39.es/ecma262/#sec-execute-async-module
void CyclicModule::execute_async_module(VM& vm)
{
    auto& realm = *vm.current_realm();

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] executing async module {}", filename());
    // 1. Assert: module.[[Status]] is evaluating or evaluating-async.
    VERIFY(m_status == ModuleStatus::Evaluating || m_status == ModuleStatus::EvaluatingAsync);
    // 2. Assert: module.[[HasTLA]] is true.
    VERIFY(m_has_top_level_await);

    // 3. Let capability be ! NewPromiseCapability(%Promise%).
    auto capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 4. Let fulfilledClosure be a new Abstract Closure with no parameters that captures module and performs the following steps when called:
    auto fulfilled_closure = [&](VM& vm) -> ThrowCompletionOr<Value> {
        // a. Perform AsyncModuleExecutionFulfilled(module).
        async_module_execution_fulfilled(vm);

        // b. Return undefined.
        return js_undefined();
    };

    // 5. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 0, "", « »).
    auto on_fulfilled = NativeFunction::create(realm, move(fulfilled_closure), 0);

    // 6. Let rejectedClosure be a new Abstract Closure with parameters (error) that captures module and performs the following steps when called:
    auto rejected_closure = [&](VM& vm) -> ThrowCompletionOr<Value> {
        auto error = vm.argument(0);

        // a. Perform AsyncModuleExecutionRejected(module, error).
        async_module_execution_rejected(vm, error);

        // b. Return undefined.
        return js_undefined();
    };

    // 7. Let onRejected be CreateBuiltinFunction(rejectedClosure, 0, "", « »).
    auto on_rejected = NativeFunction::create(realm, move(rejected_closure), 0);

    // 8. Perform PerformPromiseThen(capability.[[Promise]], onFulfilled, onRejected).
    as<Promise>(capability->promise().ptr())->perform_then(on_fulfilled, on_rejected, {});

    // 9. Perform ! module.ExecuteModule(capability).
    MUST(execute_module(vm, capability));

    // 10. Return unused.
}

// 16.2.1.5.2.3 GatherAvailableAncestors ( module, execList ), https://tc39.es/ecma262/#sec-gather-available-ancestors
void CyclicModule::gather_available_ancestors(Vector<CyclicModule*>& exec_list)
{
    // 1. For each Cyclic Module Record m of module.[[AsyncParentModules]], do
    for (auto module : m_async_parent_modules) {
        // a. If execList does not contain m and m.[[CycleRoot]].[[EvaluationError]] is empty, then
        if (!exec_list.contains_slow(module) && !module->m_cycle_root->m_evaluation_error.is_error()) {
            // i. Assert: m.[[Status]] is evaluating-async.
            VERIFY(module->m_status == ModuleStatus::EvaluatingAsync);

            // ii. Assert: m.[[EvaluationError]] is empty.
            VERIFY(!module->m_evaluation_error.is_error());

            // iii. Assert: m.[[AsyncEvaluation]] is true.
            VERIFY(module->m_async_evaluation);

            // iv. Assert: m.[[PendingAsyncDependencies]] > 0.
            VERIFY(module->m_pending_async_dependencies.value() > 0);

            // v. Set m.[[PendingAsyncDependencies]] to m.[[PendingAsyncDependencies]] - 1.
            module->m_pending_async_dependencies.value()--;

            // vi. If m.[[PendingAsyncDependencies]] = 0, then
            if (module->m_pending_async_dependencies.value() == 0) {
                // 1. Append m to execList.
                exec_list.append(module);

                // 2. If m.[[HasTLA]] is false, perform GatherAvailableAncestors(m, execList).
                if (!module->m_has_top_level_await)
                    module->gather_available_ancestors(exec_list);
            }
        }
    }

    // 2. Return unused.
}

// 16.2.1.5.2.4 AsyncModuleExecutionFulfilled ( module ), https://tc39.es/ecma262/#sec-async-module-execution-fulfilled
void CyclicModule::async_module_execution_fulfilled(VM& vm)
{
    // 1. If module.[[Status]] is evaluated, then
    if (m_status == ModuleStatus::Evaluated) {
        // a. Assert: module.[[EvaluationError]] is not empty.
        VERIFY(m_evaluation_error.is_error());

        // b. Return unused.
        return;
    }

    // 2. Assert: module.[[Status]] is evaluating-async.
    VERIFY(m_status == ModuleStatus::EvaluatingAsync);

    // 3. Assert: module.[[AsyncEvaluation]] is true.
    VERIFY(m_async_evaluation);

    // 4. Assert: module.[[EvaluationError]] is empty.
    VERIFY(!m_evaluation_error.is_error());

    // 5. Set module.[[AsyncEvaluation]] to false.
    m_async_evaluation = false;

    // 6. Set module.[[Status]] to evaluated.
    m_status = ModuleStatus::Evaluated;

    // 7. If module.[[TopLevelCapability]] is not empty, then
    if (m_top_level_capability != nullptr) {
        // a. Assert: module.[[CycleRoot]] is module.
        VERIFY(m_cycle_root == this);

        // b. Perform ! Call(module.[[TopLevelCapability]].[[Resolve]], undefined, « undefined »).
        MUST(call(vm, *m_top_level_capability->resolve(), js_undefined(), js_undefined()));
    }

    // 8. Let execList be a new empty List.
    Vector<CyclicModule*> exec_list;

    // 9. Perform GatherAvailableAncestors(module, execList).
    gather_available_ancestors(exec_list);

    // 10. Let sortedExecList be a List whose elements are the elements of execList, in the order in which they had their [[AsyncEvaluation]] fields set to true in InnerModuleEvaluation.
    // FIXME: Sort the list. To do this we need to use more than an Optional<bool> to track [[AsyncEvaluation]].

    // 11. Assert: All elements of sortedExecList have their [[AsyncEvaluation]] field set to true, [[PendingAsyncDependencies]] field set to 0, and [[EvaluationError]] field set to empty.
    VERIFY(all_of(exec_list, [&](CyclicModule* module) { return module->m_async_evaluation && module->m_pending_async_dependencies.value() == 0 && !module->m_evaluation_error.is_error(); }));

    // 12. For each Cyclic Module Record m of sortedExecList, do
    for (auto* module : exec_list) {
        // a. If m.[[Status]] is evaluated, then
        if (module->m_status == ModuleStatus::Evaluated) {
            // i. Assert: m.[[EvaluationError]] is not empty.
            VERIFY(module->m_evaluation_error.is_error());
        }
        // b. Else if m.[[HasTLA]] is true, then
        else if (module->m_has_top_level_await) {
            // i. Perform ExecuteAsyncModule(m).
            module->execute_async_module(vm);
        }
        // c. Else,
        else {
            // i. Let result be m.ExecuteModule().
            auto result = module->execute_module(vm);

            // ii. If result is an abrupt completion, then
            if (result.is_throw_completion()) {
                // 1. Perform AsyncModuleExecutionRejected(m, result.[[Value]]).
                module->async_module_execution_rejected(vm, *result.throw_completion().value());
            }
            // iii. Else,
            else {
                // 1. Set m.[[Status]] to evaluated.
                module->m_status = ModuleStatus::Evaluated;

                // 2. If m.[[TopLevelCapability]] is not empty, then
                if (module->m_top_level_capability != nullptr) {
                    // a. Assert: m.[[CycleRoot]] is m.
                    VERIFY(module->m_cycle_root == module);

                    // b. Perform ! Call(m.[[TopLevelCapability]].[[Resolve]], undefined, « undefined »).
                    MUST(call(vm, *module->m_top_level_capability->resolve(), js_undefined(), js_undefined()));
                }
            }
        }
    }

    // 13. Return unused.
}

// 16.2.1.5.2.5 AsyncModuleExecutionRejected ( module, error ), https://tc39.es/ecma262/#sec-async-module-execution-rejected
void CyclicModule::async_module_execution_rejected(VM& vm, Value error)
{
    // 1. If module.[[Status]] is evaluated, then
    if (m_status == ModuleStatus::Evaluated) {
        // a. Assert: module.[[EvaluationError]] is not empty.
        VERIFY(m_evaluation_error.is_error());

        // b. Return unused.
        return;
    }

    // 2. Assert: module.[[Status]] is evaluating-async.
    VERIFY(m_status == ModuleStatus::EvaluatingAsync);

    // 3. Assert: module.[[AsyncEvaluation]] is true.
    VERIFY(m_async_evaluation);

    // 4. Assert: module.[[EvaluationError]] is empty.
    VERIFY(!m_evaluation_error.is_error());

    // 5. Set module.[[EvaluationError]] to ThrowCompletion(error)
    m_evaluation_error = throw_completion(error);

    // 6. Set module.[[Status]] to evaluated.
    m_status = ModuleStatus::Evaluated;

    // 7. For each Cyclic Module Record m of module.[[AsyncParentModules]], do
    for (auto module : m_async_parent_modules) {
        // a. Perform AsyncModuleExecutionRejected(m, error).
        module->async_module_execution_rejected(vm, error);
    }

    // 8. If module.[[TopLevelCapability]] is not empty, then
    if (m_top_level_capability != nullptr) {
        // a. Assert: module.[[CycleRoot]] is module.
        VERIFY(m_cycle_root == this);

        // b. Perform ! Call(module.[[TopLevelCapability]].[[Reject]], undefined, « error »).
        MUST(call(vm, *m_top_level_capability->reject(), js_undefined(), error));
    }

    // 9. Return unused.
}

// 16.2.1.7 GetImportedModule ( referrer, specifier ), https://tc39.es/ecma262/#sec-GetImportedModule
GC::Ref<Module> CyclicModule::get_imported_module(ModuleRequest const& request)
{
    // 1. Assert: Exactly one element of referrer.[[LoadedModules]] is a Record whose [[Specifier]] is specifier,
    //    since LoadRequestedModules has completed successfully on referrer prior to invoking this abstract operation.
    size_t element_with_specifier_count = 0;
    for (auto const& loaded_module : m_loaded_modules) {
        if (loaded_module.specifier == request.module_specifier)
            ++element_with_specifier_count;
    }
    VERIFY(element_with_specifier_count == 1);

    for (auto const& loaded_module : m_loaded_modules) {
        if (loaded_module.specifier == request.module_specifier) {
            // 2. Let record be the Record in referrer.[[LoadedModules]] whose [[Specifier]] is specifier.
            // 3. Return record.[[Module]].
            return loaded_module.module;
        }
    }
    VERIFY_NOT_REACHED();
}

// 13.3.10.1.1 ContinueDynamicImport ( promiseCapability, moduleCompletion ), https://tc39.es/ecma262/#sec-ContinueDynamicImport
void continue_dynamic_import(GC::Ref<PromiseCapability> promise_capability, ThrowCompletionOr<GC::Ref<Module>> const& module_completion)
{
    auto& vm = promise_capability->vm();

    // 1. If moduleCompletion is an abrupt completion, then
    if (module_completion.is_throw_completion()) {
        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « moduleCompletion.[[Value]] »).
        MUST(call(vm, *promise_capability->reject(), js_undefined(), *module_completion.throw_completion().value()));

        // b. Return unused.
        return;
    }

    // 2. Let module be moduleCompletion.[[Value]].
    auto& module = *module_completion.value();

    // 3. Let loadPromise be module.LoadRequestedModules().
    auto& load_promise = module.load_requested_modules({});

    // 4. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures promiseCapability and performs the
    //    following steps when called:
    auto reject_closure = [promise_capability](VM& vm) -> ThrowCompletionOr<Value> {
        auto reason = vm.argument(0);

        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « reason »).
        MUST(call(vm, *promise_capability->reject(), js_undefined(), reason));

        // b. Return unused.
        return js_undefined();
    };

    // 5. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
    auto on_rejected = NativeFunction::create(*vm.current_realm(), move(reject_closure), 1);

    // 6. Let linkAndEvaluateClosure be a new Abstract Closure with no parameters that captures module, promiseCapability,
    //    and onRejected and performs the following steps when called:
    auto link_and_evaluate_closure = [&module, promise_capability, on_rejected](VM& vm) -> ThrowCompletionOr<Value> {
        // a. Let link be Completion(module.Link()).
        auto link = module.link(vm);

        // b. If link is an abrupt completion, then
        if (link.is_throw_completion()) {
            // i. Perform ! Call(promiseCapability.[[Reject]], undefined, « link.[[Value]] »).
            MUST(call(vm, *promise_capability->reject(), js_undefined(), *link.throw_completion().value()));

            // ii. Return unused.
            return js_undefined();
        }

        // c. Let evaluatePromise be module.Evaluate().
        auto evaluate_promise = module.evaluate(vm);

        // d. Let fulfilledClosure be a new Abstract Closure with no parameters that captures module and
        //    promiseCapability and performs the following steps when called:
        auto fulfilled_closure = [&module, promise_capability](VM& vm) -> ThrowCompletionOr<Value> {
            // i. Let namespace be GetModuleNamespace(module).
            auto namespace_ = module.get_module_namespace(vm);

            // ii. Perform ! Call(promiseCapability.[[Resolve]], undefined, « namespace »).
            MUST(call(vm, *promise_capability->resolve(), js_undefined(), namespace_.value()));

            // iii. Return unused.
            return js_undefined();
        };

        // e. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 0, "", « »).
        auto on_fulfilled = NativeFunction::create(*vm.current_realm(), move(fulfilled_closure), 0);

        // f. Perform PerformPromiseThen(evaluatePromise, onFulfilled, onRejected).
        evaluate_promise.value()->perform_then(on_fulfilled, on_rejected, {});

        // g. Return unused.
        return js_undefined();
    };

    // 7. Let linkAndEvaluate be CreateBuiltinFunction(linkAndEvaluateClosure, 0, "", « »).
    auto link_and_evaluate = NativeFunction::create(*vm.current_realm(), move(link_and_evaluate_closure), 0);

    // 8. Perform PerformPromiseThen(loadPromise, linkAndEvaluate, onRejected).
    // FIXME: This is likely a spec bug, see load_requested_modules.
    as<Promise>(*load_promise.promise()).perform_then(link_and_evaluate, on_rejected, {});

    // 9. Return unused.
}

}
