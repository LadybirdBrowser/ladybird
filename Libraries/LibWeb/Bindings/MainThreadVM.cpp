/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022-2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/DeferGC.h>
#include <LibJS/AST.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/FinalizationRegistry.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ShadowRealm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/SourceTextModule.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/SyntheticHostDefined.h>
#include <LibWeb/Bindings/WindowExposedInterfaces.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/PromiseRejectionEvent.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/Script.h>
#include <LibWeb/HTML/Scripting/SyntheticRealmSettings.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/ShadowRealmGlobalScope.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HTML/WorkletGlobalScope.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Bindings {

static RefPtr<JS::VM> s_main_thread_vm;

// https://html.spec.whatwg.org/multipage/webappapis.html#active-script
HTML::Script* active_script()
{
    // 1. Let record be GetActiveScriptOrModule().
    auto record = main_thread_vm().get_active_script_or_module();

    // 2. If record is null, return null.
    // 3. Return record.[[HostDefined]].
    return record.visit(
        [](GC::Ref<JS::Script>& js_script) -> HTML::Script* {
            return as<HTML::ClassicScript>(js_script->host_defined());
        },
        [](GC::Ref<JS::Module>& js_module) -> HTML::Script* {
            return as<HTML::ModuleScript>(js_module->host_defined());
        },
        [](Empty) -> HTML::Script* {
            return nullptr;
        });
}

ErrorOr<void> initialize_main_thread_vm(HTML::EventLoop::Type type)
{
    VERIFY(!s_main_thread_vm);

    s_main_thread_vm = TRY(JS::VM::create(make<WebEngineCustomData>()));

    auto& custom_data = as<WebEngineCustomData>(*s_main_thread_vm->custom_data());
    custom_data.agent.event_loop = s_main_thread_vm->heap().allocate<HTML::EventLoop>(type);

    s_main_thread_vm->on_unimplemented_property_access = [](auto const& object, auto const& property_key) {
        dbgln("FIXME: Unimplemented IDL interface: '{}.{}'", object.class_name(), property_key.to_string());
    };

    // NOTE: We intentionally leak the main thread JavaScript VM.
    //       This avoids doing an exhaustive garbage collection on process exit.
    s_main_thread_vm->ref();

    // 8.1.5.1 HostEnsureCanAddPrivateElement(O), https://html.spec.whatwg.org/multipage/webappapis.html#the-hostensurecanaddprivateelement-implementation
    s_main_thread_vm->host_ensure_can_add_private_element = [](JS::Object const& object) -> JS::ThrowCompletionOr<void> {
        // 1. If O is a WindowProxy object, or implements Location, then return Completion { [[Type]]: throw, [[Value]]: a new TypeError }.
        if (is<HTML::WindowProxy>(object) || is<HTML::Location>(object))
            return s_main_thread_vm->throw_completion<JS::TypeError>("Cannot add private elements to window or location object"sv);

        // 2. Return NormalCompletion(unused).
        return {};
    };

    // FIXME: Implement 8.1.5.2 HostEnsureCanCompileStrings(callerRealm, calleeRealm), https://html.spec.whatwg.org/multipage/webappapis.html#hostensurecancompilestrings(callerrealm,-calleerealm)

    // 8.1.5.3 HostPromiseRejectionTracker(promise, operation), https://html.spec.whatwg.org/multipage/webappapis.html#the-hostpromiserejectiontracker-implementation
    // https://whatpr.org/html/9893/webappapis.html#the-hostpromiserejectiontracker-implementation
    s_main_thread_vm->host_promise_rejection_tracker = [](JS::Promise& promise, JS::Promise::RejectionOperation operation) {
        auto& vm = *s_main_thread_vm;

        // 1. Let script be the running script.
        //    The running script is the script in the [[HostDefined]] field in the ScriptOrModule component of the running JavaScript execution context.
        HTML::Script* script { nullptr };
        vm.running_execution_context().script_or_module.visit(
            [&script](GC::Ref<JS::Script>& js_script) {
                script = as<HTML::ClassicScript>(js_script->host_defined());
            },
            [&script](GC::Ref<JS::Module>& js_module) {
                script = as<HTML::ModuleScript>(js_module->host_defined());
            },
            [](Empty) {
            });

        // 2. If script is a classic script and script's muted errors is true, then return.
        // NOTE: is<T>() returns false if nullptr is passed.
        if (is<HTML::ClassicScript>(script)) {
            auto const& classic_script = static_cast<HTML::ClassicScript const&>(*script);
            if (classic_script.muted_errors() == HTML::ClassicScript::MutedErrors::Yes)
                return;
        }

        // 3. Let realm be the current realm.
        // 4. If script is not null, then set settings object to script's realm.
        auto& realm = script ? script->realm() : *vm.current_realm();

        // 5. Let global be realm's global object.
        auto* global_mixin = dynamic_cast<HTML::UniversalGlobalScopeMixin*>(&realm.global_object());
        VERIFY(global_mixin);
        auto& global = global_mixin->this_impl();

        switch (operation) {
        // 6. If operation is "reject",
        case JS::Promise::RejectionOperation::Reject:
            // 1. Append promise to global's about-to-be-notified rejected promises list.
            global_mixin->push_onto_about_to_be_notified_rejected_promises_list(promise);
            break;
        // 7. If operation is "handle",
        case JS::Promise::RejectionOperation::Handle: {
            // 1. If global's about-to-be-notified rejected promises list contains promise, then remove promise from that list and return.
            bool removed_about_to_be_notified_rejected_promise = global_mixin->remove_from_about_to_be_notified_rejected_promises_list(promise);
            if (removed_about_to_be_notified_rejected_promise)
                return;

            // 3. Remove promise from global's outstanding rejected promises weak set.
            bool removed_outstanding_rejected_promise = global_mixin->remove_from_outstanding_rejected_promises_weak_set(&promise);

            // 2. If global's outstanding rejected promises weak set does not contain promise, then return.
            // NOTE: This is done out of order because removed_outstanding_rejected_promise will be false if the promise wasn't in the set or true if it was and got removed.
            if (!removed_outstanding_rejected_promise)
                return;

            // 4. Queue a global task on the DOM manipulation task source given global to fire an event named rejectionhandled at global, using PromiseRejectionEvent,
            //    with the promise attribute initialized to promise, and the reason attribute initialized to the value of promise's [[PromiseResult]] internal slot.
            HTML::queue_global_task(HTML::Task::Source::DOMManipulation, global, GC::create_function(s_main_thread_vm->heap(), [&global, &promise] {
                // FIXME: This currently assumes that global is a WindowObject.
                auto& window = as<HTML::Window>(global);

                HTML::PromiseRejectionEventInit event_init {
                    {}, // Initialize the inherited DOM::EventInit
                    /* .promise = */ promise,
                    /* .reason = */ promise.result(),
                };
                auto promise_rejection_event = HTML::PromiseRejectionEvent::create(HTML::relevant_realm(global), HTML::EventNames::rejectionhandled, event_init);
                window.dispatch_event(promise_rejection_event);
            }));
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    };

    // 8.1.5.4.1 HostCallJobCallback(callback, V, argumentsList), https://html.spec.whatwg.org/multipage/webappapis.html#hostcalljobcallback
    // https://whatpr.org/html/9893/webappapis.html#hostcalljobcallback
    s_main_thread_vm->host_call_job_callback = [](JS::JobCallback& callback, JS::Value this_value, ReadonlySpan<JS::Value> arguments_list) {
        auto& callback_host_defined = as<WebEngineCustomJobCallbackData>(*callback.custom_data());

        // 1. Let incumbent realm be callback.[[HostDefined]].[[IncumbentRealm]].
        auto& incumbent_realm = callback_host_defined.incumbent_realm;

        // 2. Let script execution context be callback.[[HostDefined]].[[ActiveScriptContext]].
        auto* script_execution_context = callback_host_defined.active_script_context.ptr();

        // 3. Prepare to run a callback with incumbent realm.
        HTML::prepare_to_run_callback(incumbent_realm);

        // 4. If script execution context is not null, then push script execution context onto the JavaScript execution context stack.
        if (script_execution_context)
            s_main_thread_vm->push_execution_context(*script_execution_context);

        // 5. Let result be Call(callback.[[Callback]], V, argumentsList).
        auto result = JS::call(*s_main_thread_vm, callback.callback(), this_value, arguments_list);

        // 6. If script execution context is not null, then pop script execution context from the JavaScript execution context stack.
        if (script_execution_context) {
            VERIFY(&s_main_thread_vm->running_execution_context() == script_execution_context);
            s_main_thread_vm->pop_execution_context();
        }

        // 7. Clean up after running a callback with incumbent realm.
        HTML::clean_up_after_running_callback(incumbent_realm);

        // 8. Return result.
        return result;
    };

    // 8.1.5.4.2 HostEnqueueFinalizationRegistryCleanupJob(finalizationRegistry), https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuefinalizationregistrycleanupjob
    s_main_thread_vm->host_enqueue_finalization_registry_cleanup_job = [](JS::FinalizationRegistry& finalization_registry) {
        // 1. Let global be finalizationRegistry.[[Realm]]'s global object.
        auto& global = finalization_registry.realm().global_object();

        // 2. Queue a global task on the JavaScript engine task source given global to perform the following steps:
        HTML::queue_global_task(HTML::Task::Source::JavaScriptEngine, global, GC::create_function(s_main_thread_vm->heap(), [&finalization_registry] {
            // 1. Let entry be finalizationRegistry.[[CleanupCallback]].[[Callback]].[[Realm]].
            auto& entry = *finalization_registry.cleanup_callback().callback().realm();

            // 2. Check if we can run script with entry. If this returns "do not run", then return.
            if (HTML::can_run_script(entry) == HTML::RunScriptDecision::DoNotRun)
                return;

            // 3. Prepare to run script with entry.
            HTML::prepare_to_run_script(entry);

            // 4. Let result be the result of performing CleanupFinalizationRegistry(finalizationRegistry).
            auto result = finalization_registry.cleanup();

            // 5. Clean up after running script with entry.
            HTML::clean_up_after_running_script(entry);

            // 6. If result is an abrupt completion, then report the exception given by result.[[Value]].
            if (result.is_error())
                HTML::report_exception(result, entry);
        }));
    };

    // 8.1.5.4.3 HostEnqueuePromiseJob(job, realm), https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuepromisejob
    // // https://whatpr.org/html/9893/webappapis.html#hostenqueuepromisejob
    s_main_thread_vm->host_enqueue_promise_job = [](GC::Ref<GC::Function<JS::ThrowCompletionOr<JS::Value>()>> job, JS::Realm* realm) {
        auto& vm = *s_main_thread_vm;

        // IMPLEMENTATION DEFINED: The JS spec says we must take implementation defined steps to make the currently active script or module at the time of HostEnqueuePromiseJob being invoked
        //                         also be the active script or module of the job at the time of its invocation.
        //                         This means taking it here now and passing it through to the lambda.
        auto script_or_module = vm.get_active_script_or_module();

        // 1. Queue a microtask to perform the following steps:
        // This instance of "queue a microtask" uses the "implied document". The best fit for "implied document" here is "If the task is being queued by or for a script, then return the script's settings object's responsible document."
        // Do note that "implied document" from the spec is handwavy and the spec authors are trying to get rid of it: https://github.com/whatwg/html/issues/4980
        auto* script = active_script();

        auto& heap = realm ? realm->heap() : vm.heap();
        HTML::queue_a_microtask(script ? script->settings_object().responsible_document().ptr() : nullptr, GC::create_function(heap, [&vm, realm, job = move(job), script_or_module = move(script_or_module)] {
            // The dummy execution context has to be kept up here to keep it alive for the duration of the function.
            OwnPtr<JS::ExecutionContext> dummy_execution_context;

            if (realm) {
                // 1. If realm is not null, then check if we can run script with realm. If this returns "do not run" then return.
                if (HTML::can_run_script(*realm) == HTML::RunScriptDecision::DoNotRun)
                    return;

                // 2. If realm is not null, then prepare to run script with realm.
                HTML::prepare_to_run_script(*realm);

                // IMPLEMENTATION DEFINED: Additionally to preparing to run a script, we also prepare to run a callback here. This matches WebIDL's
                //                         invoke_callback() / call_user_object_operation() functions, and prevents a crash in host_make_job_callback()
                //                         when getting the incumbent settings object.
                HTML::prepare_to_run_callback(*realm);

                // IMPLEMENTATION DEFINED: Per the previous "implementation defined" comment, we must now make the script or module the active script or module.
                //                         Since the only active execution context currently is the realm execution context of job settings, lets attach it here.
                HTML::execution_context_of_realm(*realm).script_or_module = script_or_module;
            } else {
                // FIXME: We need to setup a dummy execution context in case a JS::NativeFunction is called when processing the job.
                //        This is because JS::NativeFunction::call excepts something to be on the execution context stack to be able to get the caller context to initialize the environment.
                //        Do note that the JS spec gives _no_ guarantee that the execution context stack has something on it if HostEnqueuePromiseJob was called with a null realm: https://tc39.es/ecma262/#job-preparedtoevaluatecode
                dummy_execution_context = JS::ExecutionContext::create();
                dummy_execution_context->script_or_module = script_or_module;
                vm.push_execution_context(*dummy_execution_context);
            }

            // 3. Let result be job().
            auto result = job->function()();

            // 4. If realm is not null, then clean up after running script with job settings.
            if (realm) {
                // IMPLEMENTATION DEFINED: Disassociate the realm execution context from the script or module.
                HTML::execution_context_of_realm(*realm).script_or_module = Empty {};

                // IMPLEMENTATION DEFINED: See comment above, we need to clean up the non-standard prepare_to_run_callback() call.
                HTML::clean_up_after_running_callback(*realm);

                HTML::clean_up_after_running_script(*realm);
            } else {
                // Pop off the dummy execution context. See the above FIXME block about why this is done.
                vm.pop_execution_context();
            }

            // 5. If result is an abrupt completion, then report the exception given by result.[[Value]].
            if (result.is_error())
                HTML::report_exception(result, *realm);
        }));
    };

    // 8.1.5.4.4 HostMakeJobCallback(callable), https://html.spec.whatwg.org/multipage/webappapis.html#hostmakejobcallback
    // https://whatpr.org/html/9893/webappapis.html#hostmakejobcallback
    s_main_thread_vm->host_make_job_callback = [](JS::FunctionObject& callable) -> GC::Ref<JS::JobCallback> {
        // 1. Let incumbent realm be the incumbent realm.
        auto& incumbent_realm = HTML::incumbent_realm();

        // 2. Let active script be the active script.
        auto* script = active_script();

        // 3. Let script execution context be null.
        OwnPtr<JS::ExecutionContext> script_execution_context;

        // 4. If active script is not null, set script execution context to a new JavaScript execution context, with its Function field set to null,
        //    its Realm field set to active script's realm, and its ScriptOrModule set to active script's record.
        if (script) {
            script_execution_context = JS::ExecutionContext::create();
            script_execution_context->function = nullptr;
            script_execution_context->realm = &script->realm();
            if (is<HTML::ClassicScript>(script)) {
                script_execution_context->script_or_module = GC::Ref<JS::Script>(*as<HTML::ClassicScript>(script)->script_record());
            } else if (is<HTML::ModuleScript>(script)) {
                if (is<HTML::JavaScriptModuleScript>(script)) {
                    script_execution_context->script_or_module = GC::Ref<JS::Module>(*as<HTML::JavaScriptModuleScript>(script)->record());
                } else {
                    // NOTE: Handle CSS and JSON module scripts once we have those.
                    VERIFY_NOT_REACHED();
                }
            } else {
                VERIFY_NOT_REACHED();
            }
        }

        // 5. Return the JobCallback Record { [[Callback]]: callable, [[HostDefined]]: { [[IncumbentRealm]]: incumbent realm, [[ActiveScriptContext]]: script execution context } }.
        auto host_defined = adopt_own(*new WebEngineCustomJobCallbackData(incumbent_realm, move(script_execution_context)));
        return JS::JobCallback::create(*s_main_thread_vm, callable, move(host_defined));
    };

    // 8.1.6.7.1 HostGetImportMetaProperties(moduleRecord), https://html.spec.whatwg.org/multipage/webappapis.html#hostgetimportmetaproperties
    s_main_thread_vm->host_get_import_meta_properties = [](JS::SourceTextModule& module_record) {
        auto& realm = module_record.realm();
        auto& vm = realm.vm();

        // 1. Let moduleScript be moduleRecord.[[HostDefined]].
        auto& module_script = *as<HTML::Script>(module_record.host_defined());

        // 2. Assert: moduleScript's base URL is not null, as moduleScript is a JavaScript module script.
        VERIFY(module_script.base_url().has_value());

        // 3. Let urlString be moduleScript's base URL, serialized.
        auto url_string = module_script.base_url()->serialize();

        // 4. Let steps be the following steps, given the argument specifier:
        auto steps = [module_script = GC::Ref { module_script }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto specifier = vm.argument(0);

            // 1. Set specifier to ? ToString(specifier).
            auto specifier_string = TRY(specifier.to_string(vm));

            // 2. Let url be the result of resolving a module specifier given moduleScript and specifier.
            auto url = TRY(Bindings::throw_dom_exception_if_needed(vm, [&] {
                return HTML::resolve_module_specifier(*module_script, specifier_string);
            }));

            // 3. Return the serialization of url.
            return JS::PrimitiveString::create(vm, url.serialize());
        };

        // 4. Let resolveFunction be ! CreateBuiltinFunction(steps, 1, "resolve", « »).
        auto resolve_function = JS::NativeFunction::create(realm, move(steps), 1, vm.names.resolve);

        // 5. Return « Record { [[Key]]: "url", [[Value]]: urlString }, Record { [[Key]]: "resolve", [[Value]]: resolveFunction } ».
        HashMap<JS::PropertyKey, JS::Value> meta;
        meta.set("url"_fly_string, JS::PrimitiveString::create(vm, move(url_string)));
        meta.set("resolve"_fly_string, resolve_function);

        return meta;
    };

    // 8.1.6.7.2 HostGetSupportedImportAttributes(), https://html.spec.whatwg.org/multipage/webappapis.html#hostgetsupportedimportassertions
    s_main_thread_vm->host_get_supported_import_attributes = []() -> Vector<String> {
        // 1. Return « "type" ».
        return { "type"_string };
    };

    // 8.1.6.7.3 HostLoadImportedModule(referrer, moduleRequest, loadState, payload), https://html.spec.whatwg.org/multipage/webappapis.html#hostloadimportedmodule
    // https://whatpr.org/html/9893/webappapis.html#hostloadimportedmodule
    s_main_thread_vm->host_load_imported_module = [](JS::ImportedModuleReferrer referrer, JS::ModuleRequest const& module_request, GC::Ptr<JS::GraphLoadingState::HostDefined> load_state, JS::ImportedModulePayload payload) -> void {
        auto& vm = *s_main_thread_vm;

        // 1. Let moduleMapRealm be the current realm.
        auto* module_map_realm = vm.current_realm();

        // 2. If moduleMapRealm's global object implements WorkletGlobalScope or ServiceWorkerGlobalScope and loadState is undefined, then:
        if ((is<HTML::WorkletGlobalScope>(module_map_realm->global_object()) || is<ServiceWorker::ServiceWorkerGlobalScope>(module_map_realm->global_object())) && !load_state) {
            // 1. Let completion be Completion Record { [[Type]]: throw, [[Value]]: a new TypeError, [[Target]]: empty }.
            auto completion = JS::throw_completion(JS::TypeError::create(*module_map_realm, "Dynamic Import not available for Worklets or ServiceWorkers"_string));

            // 2. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, completion).
            JS::finish_loading_imported_module(referrer, module_request, payload, completion);

            // 3. Return.
            return;
        }

        // 3. Let referencingScript be null.
        Optional<HTML::Script&> referencing_script;

        // 4. Let originalFetchOptions be the default script fetch options.
        auto original_fetch_options = HTML::default_script_fetch_options();

        // 5. Let fetchReferrer be "client".
        Fetch::Infrastructure::Request::ReferrerType fetch_referrer = Fetch::Infrastructure::Request::Referrer::Client;

        // 6. If referrer is a Script Record or a Cyclic Module Record, then:
        if (referrer.has<GC::Ref<JS::Script>>() || referrer.has<GC::Ref<JS::CyclicModule>>()) {
            // 1. Set referencingScript to referrer.[[HostDefined]].
            referencing_script = as<HTML::Script>(referrer.has<GC::Ref<JS::Script>>() ? *referrer.get<GC::Ref<JS::Script>>()->host_defined() : *referrer.get<GC::Ref<JS::CyclicModule>>()->host_defined());

            // 2. Set fetchReferrer to referencingScript's base URL.
            fetch_referrer = referencing_script->base_url().value();

            // FIXME: 3. Set originalFetchOptions to referencingScript's fetch options.

            // 4. Set moduleMapRealm to referencingScript's realm.
            module_map_realm = &referencing_script->realm();
        }

        // 7. If referrer is a Cyclic Module Record and moduleRequest is equal to the first element of referrer.[[RequestedModules]], then:
        if (referrer.has<GC::Ref<JS::CyclicModule>>()) {
            // FIXME: Why do we need to check requested modules is empty here?
            if (auto const& requested_modules = referrer.get<GC::Ref<JS::CyclicModule>>()->requested_modules(); !requested_modules.is_empty() && module_request == requested_modules.first()) {
                // 1. For each ModuleRequest record requested of referrer.[[RequestedModules]]:
                for (auto const& module_request : referrer.get<GC::Ref<JS::CyclicModule>>()->requested_modules()) {
                    // 1. If moduleRequest.[[Attributes]] contains a Record entry such that entry.[[Key]] is not "type", then:
                    for (auto const& attribute : module_request.attributes) {
                        if (attribute.key == "type"sv)
                            continue;

                        // 1. Let completion be Completion Record { [[Type]]: throw, [[Value]]: a new SyntaxError exception, [[Target]]: empty }.
                        auto completion = JS::throw_completion(JS::SyntaxError::create(*module_map_realm, "Module request attributes must only contain a type attribute"_string));

                        // 2. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, completion).
                        JS::finish_loading_imported_module(referrer, module_request, payload, completion);

                        // 3. Return.
                        return;
                    }
                }

                // 2. Resolve a module specifier given referencingScript and moduleRequest.[[Specifier]], catching any
                //    exceptions. If they throw an exception, let resolutionError be the thrown exception.
                auto maybe_exception = HTML::resolve_module_specifier(referencing_script, module_request.module_specifier.to_string());

                // 3. If the previous step threw an exception, then:
                if (maybe_exception.is_exception()) {
                    // 1. Let completion be Completion Record { [[Type]]: throw, [[Value]]: resolutionError, [[Target]]: empty }.
                    auto completion = exception_to_throw_completion(main_thread_vm(), maybe_exception.exception());

                    // 2. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, completion).
                    JS::finish_loading_imported_module(referrer, module_request, payload, completion);

                    // 3. Return.
                    return;
                }

                // 4. Let moduleType be the result of running the module type from module request steps given moduleRequest.
                auto module_type = HTML::module_type_from_module_request(module_request);

                // 5. If the result of running the module type allowed steps given moduleType and moduleMapRealm is false, then:
                if (!HTML::module_type_allowed(*module_map_realm, module_type)) {
                    // 1. Let completion be Completion Record { [[Type]]: throw, [[Value]]: a new TypeError exception, [[Target]]: empty }.
                    auto completion = JS::throw_completion(JS::SyntaxError::create(*module_map_realm, MUST(String::formatted("Module type '{}' is not supported", module_type))));

                    // 2. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, completion).
                    JS::finish_loading_imported_module(referrer, module_request, payload, completion);

                    // 3. Return
                    return;
                }

                // Spec-Note: This step is essentially validating all of the requested module specifiers and type attributes
                //            when the first call to HostLoadImportedModule for a static module dependency list is made, to
                //            avoid further loading operations in the case any one of the dependencies has a static error.
                //            We treat a module with unresolvable module specifiers or unsupported type attributes the same
                //            as one that cannot be parsed; in both cases, a syntactic issue makes it impossible to ever
                //            contemplate linking the module later.
            }
        }

        // 8. Let url be the result of resolving a module specifier given referencingScript and moduleRequest.[[Specifier]],
        //    catching any exceptions. If they throw an exception, let resolutionError be the thrown exception.
        auto url = HTML::resolve_module_specifier(referencing_script, module_request.module_specifier.to_string());

        // 9. If the previous step threw an exception, then:
        if (url.is_exception()) {
            // 1. Let completion be Completion Record { [[Type]]: throw, [[Value]]: resolutionError, [[Target]]: empty }.
            auto completion = exception_to_throw_completion(main_thread_vm(), url.exception());

            // 2. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, completion).
            HTML::TemporaryExecutionContext context { *module_map_realm };
            JS::finish_loading_imported_module(referrer, module_request, payload, completion);

            // 3. Return.
            return;
        }

        // 10. Let settingsObject be moduleMapRealm's principal realm's settings object.
        auto& settings_object = HTML::principal_realm_settings_object(HTML::principal_realm(*module_map_realm));

        // 11. Let fetchOptions be the result of getting the descendant script fetch options given originalFetchOptions, url, and settingsObject.
        auto fetch_options = HTML::get_descendant_script_fetch_options(original_fetch_options, url.value(), settings_object);

        // 12. Let destination be "script".
        auto destination = Fetch::Infrastructure::Request::Destination::Script;

        // 13. Let fetchClient be moduleMapRealm's principal realm's settings object.
        GC::Ref fetch_client { HTML::principal_realm_settings_object(HTML::principal_realm(*module_map_realm)) };

        // 15. If loadState is not undefined, then:
        HTML::PerformTheFetchHook perform_fetch;
        if (load_state) {
            auto& fetch_context = static_cast<HTML::FetchContext&>(*load_state);

            // 1. Set destination to loadState.[[Destination]].
            destination = fetch_context.destination;

            // 2. Set fetchClient to loadState.[[FetchClient]].
            fetch_client = fetch_context.fetch_client;

            // For step 13
            perform_fetch = fetch_context.perform_fetch;
        }

        auto on_single_fetch_complete = HTML::create_on_fetch_script_complete(module_map_realm->heap(), [referrer, module_map_realm, load_state, module_request, payload](GC::Ptr<HTML::Script> const& module_script) -> void {
            auto& realm = *module_map_realm;
            // onSingleFetchComplete given moduleScript is the following algorithm:
            // 1. Let completion be null.
            // NOTE: Our JS::Completion does not support non JS::Value types for its [[Value]], a such we
            //       use JS::ThrowCompletionOr here.

            auto& vm = realm.vm();
            GC::Ptr<JS::Module> module = nullptr;

            auto completion = [&]() -> JS::ThrowCompletionOr<GC::Ref<JS::Module>> {
                // 2. If moduleScript is null, then set completion to Completion Record { [[Type]]: throw, [[Value]]: a new TypeError, [[Target]]: empty }.
                if (!module_script) {
                    return JS::throw_completion(JS::TypeError::create(realm, ByteString::formatted("Loading imported module '{}' failed.", module_request.module_specifier)));
                }
                // 3. Otherwise, if moduleScript's parse error is not null, then:
                else if (!module_script->parse_error().is_null()) {
                    // 1. Let parseError be moduleScript's parse error.
                    auto parse_error = module_script->parse_error();

                    // 2. Set completion to Completion Record { [[Type]]: throw, [[Value]]: parseError, [[Target]]: empty }.
                    auto completion = JS::throw_completion(parse_error);

                    // 3. If loadState is not undefined and loadState.[[ParseError]] is null, set loadState.[[ParseError]] to parseError.
                    if (load_state) {
                        auto& load_state_as_fetch_context = static_cast<HTML::FetchContext&>(*load_state);
                        if (load_state_as_fetch_context.parse_error.is_null()) {
                            load_state_as_fetch_context.parse_error = parse_error;
                        }
                    }

                    return completion;
                }
                // 4. Otherwise, set completion to Completion Record { [[Type]]: normal, [[Value]]: moduleScript's record, [[Target]]: empty }.
                else {
                    module = static_cast<HTML::JavaScriptModuleScript&>(*module_script).record();
                    return JS::ThrowCompletionOr<GC::Ref<JS::Module>>(*module);
                }
            }();

            // 5. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, completion).
            // NON-STANDARD: To ensure that LibJS can find the module on the stack, we push a new execution context.

            auto module_execution_context = JS::ExecutionContext::create();
            module_execution_context->realm = realm;
            if (module)
                module_execution_context->script_or_module = GC::Ref { *module };
            vm.push_execution_context(*module_execution_context);

            JS::finish_loading_imported_module(referrer, module_request, payload, completion);

            vm.pop_execution_context();
        });

        // 16. Fetch a single imported module script given url, fetchClient, destination, fetchOptions, moduleMapRealm, fetchReferrer,
        //     moduleRequest, and onSingleFetchComplete as defined below.
        //     If loadState is not undefined and loadState.[[PerformFetch]] is not null, pass loadState.[[PerformFetch]] along as well.
        HTML::fetch_single_imported_module_script(*module_map_realm, url.release_value(), *fetch_client, destination, fetch_options, *module_map_realm, fetch_referrer, module_request, perform_fetch, on_single_fetch_complete);
    };

    // https://whatpr.org/html/9893/webappapis.html#hostinitializeshadowrealm(realm,-context,-o)
    // 8.1.6.8 HostInitializeShadowRealm(realm, context, O)
    s_main_thread_vm->host_initialize_shadow_realm = [](JS::Realm& realm, NonnullOwnPtr<JS::ExecutionContext> context, JS::ShadowRealm& object) -> JS::ThrowCompletionOr<void> {
        // FIXME: 1. Set realm's is global prototype chain mutable to true.

        // 2. Let globalObject be a new ShadowRealmGlobalScope object with realm.
        auto global_object = HTML::ShadowRealmGlobalScope::create(realm);

        // 3. Let settings be a new synthetic realm settings object that this algorithm will subsequently initialize.
        auto settings = HTML::SyntheticRealmSettings {
            // 4. Set settings's execution context to context.
            .execution_context = move(context),

            // 5. Set settings's principal realm to O's associated realm's principal realm
            .principal_realm = HTML::principal_realm(object.shape().realm()),

            // 6. Set settings's module map to a new module map, initially empty.
            .module_map = realm.create<HTML::ModuleMap>(),
        };

        // 7. Set realm.[[HostDefined]] to settings.
        realm.set_host_defined(make<Bindings::SyntheticHostDefined>(move(settings), realm.create<Bindings::Intrinsics>(realm)));

        // 8. Set realm.[[GlobalObject]] to globalObject.
        realm.set_global_object(global_object);

        // 9. Set realm.[[GlobalEnv]] to NewGlobalEnvironment(globalObject, globalObject).
        realm.set_global_environment(realm.heap().allocate<JS::GlobalEnvironment>(global_object, global_object));

        // 10. Perform ? SetDefaultGlobalBindings(realm).
        set_default_global_bindings(realm);

        // NOTE: This needs to be done after initialization so that the realm has an intrinsics in its [[HostDefined]]
        global_object->initialize_web_interfaces();

        // 11. Return NormalCompletion(unused).
        return {};
    };

    s_main_thread_vm->host_unrecognized_date_string = [](StringView date) {
        dbgln("Unable to parse date string: \"{}\"", date);
    };

    return {};
}

JS::VM& main_thread_vm()
{
    VERIFY(s_main_thread_vm);
    return *s_main_thread_vm;
}

// https://dom.spec.whatwg.org/#queue-a-mutation-observer-compound-microtask
void queue_mutation_observer_microtask(DOM::Document const& document)
{
    auto& vm = main_thread_vm();
    auto& surrounding_agent = as<WebEngineCustomData>(*vm.custom_data()).agent;

    // 1. If the surrounding agent’s mutation observer microtask queued is true, then return.
    if (surrounding_agent.mutation_observer_microtask_queued)
        return;

    // 2. Set the surrounding agent’s mutation observer microtask queued to true.
    surrounding_agent.mutation_observer_microtask_queued = true;

    // 3. Queue a microtask to notify mutation observers.
    // NOTE: This uses the implied document concept. In the case of mutation observers, it is always done in a node context, so document should be that node's document.
    HTML::queue_a_microtask(&document, GC::create_function(vm.heap(), [&surrounding_agent, &heap = document.heap()]() {
        // 1. Set the surrounding agent’s mutation observer microtask queued to false.
        surrounding_agent.mutation_observer_microtask_queued = false;

        // 2. Let notifySet be a clone of the surrounding agent’s mutation observers.
        GC::RootVector<DOM::MutationObserver*> notify_set(heap);
        for (auto& observer : surrounding_agent.mutation_observers)
            notify_set.append(&observer);

        // 3. Let signalSet be a clone of the surrounding agent’s signal slots.
        // 4. Empty the surrounding agent’s signal slots.
        auto signal_set = move(surrounding_agent.signal_slots);

        // 5. For each mo of notifySet:
        for (auto& mutation_observer : notify_set) {
            // 1. Let records be a clone of mo’s record queue.
            // 2. Empty mo’s record queue.
            auto records = mutation_observer->take_records();

            // 3. For each node of mo’s node list, remove all transient registered observers whose observer is mo from node’s registered observer list.
            for (auto& node : mutation_observer->node_list()) {
                // FIXME: Is this correct?
                if (node.is_null())
                    continue;

                if (node->registered_observer_list()) {
                    node->registered_observer_list()->remove_all_matching([&mutation_observer](DOM::RegisteredObserver& registered_observer) {
                        return is<DOM::TransientRegisteredObserver>(registered_observer) && static_cast<DOM::TransientRegisteredObserver&>(registered_observer).observer().ptr() == mutation_observer;
                    });
                }
            }

            // 4. If records is not empty, then invoke mo’s callback with « records, mo » and "report", and with callback this value mo.
            if (!records.is_empty()) {
                auto& callback = mutation_observer->callback();
                auto& realm = callback.callback_context;

                auto wrapped_records = MUST(JS::Array::create(realm, 0));
                for (size_t i = 0; i < records.size(); ++i) {
                    auto& record = records.at(i);
                    auto property_index = JS::PropertyKey { i };
                    MUST(wrapped_records->create_data_property(property_index, record.ptr()));
                }

                (void)WebIDL::invoke_callback(callback, mutation_observer, WebIDL::ExceptionBehavior::Report, wrapped_records, mutation_observer);
            }
        }

        // 6. For each slot of signalSet, fire an event named slotchange, with its bubbles attribute set to true, at slot.
        for (auto& slot : signal_set) {
            DOM::EventInit event_init;
            event_init.bubbles = true;
            slot->dispatch_event(DOM::Event::create(slot->realm(), HTML::EventNames::slotchange, event_init));
        }
    }));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#creating-a-new-javascript-realm
NonnullOwnPtr<JS::ExecutionContext> create_a_new_javascript_realm(JS::VM& vm, Function<JS::Object*(JS::Realm&)> create_global_object, Function<JS::Object*(JS::Realm&)> create_global_this_value)
{
    // 1. Perform InitializeHostDefinedRealm() with the provided customizations for creating the global object and the global this binding.
    // 2. Let realm execution context be the running JavaScript execution context.
    auto realm_execution_context = MUST(JS::Realm::initialize_host_defined_realm(vm, move(create_global_object), move(create_global_this_value)));

    // 3. Remove realm execution context from the JavaScript execution context stack.
    vm.execution_context_stack().remove_first_matching([&realm_execution_context](auto execution_context) {
        return execution_context == realm_execution_context.ptr();
    });

    // NO-OP: 4. Let realm be realm execution context's Realm component.
    // NO-OP: 5. Set realm's agent to agent.

    // FIXME: 6. If agent's agent cluster's cross-origin isolation mode is "none", then:
    //          1. Let global be realm's global object.
    //          2. Let status be ! global.[[Delete]]("SharedArrayBuffer").
    //          3. Assert: status is true.

    // 7. Return realm execution context.
    return realm_execution_context;
}

void WebEngineCustomData::spin_event_loop_until(GC::Root<GC::Function<bool()>> goal_condition)
{
    Platform::EventLoopPlugin::the().spin_until(move(goal_condition));
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#invoke-custom-element-reactions
void invoke_custom_element_reactions(Vector<GC::Root<DOM::Element>>& element_queue)
{
    // 1. While queue is not empty:
    while (!element_queue.is_empty()) {
        // 1. Let element be the result of dequeuing from queue.
        auto element = element_queue.take_first();

        // 2. Let reactions be element's custom element reaction queue.
        auto* reactions = element->custom_element_reaction_queue();

        // 3. Repeat until reactions is empty:
        if (!reactions)
            continue;
        while (!reactions->is_empty()) {
            // 1. Remove the first element of reactions, and let reaction be that element. Switch on reaction's type:
            auto reaction = reactions->take_first();

            reaction.visit(
                [&](DOM::CustomElementUpgradeReaction const& custom_element_upgrade_reaction) -> void {
                    // -> upgrade reaction
                    //      Upgrade element using reaction's custom element definition.
                    auto maybe_exception = element->upgrade_element(*custom_element_upgrade_reaction.custom_element_definition);
                    // If this throws an exception, catch it, and report it for reaction's custom element definition's constructor's corresponding JavaScript object's associated realm's global object.
                    if (maybe_exception.is_error()) {
                        // FIXME: Should it be easier to get to report an exception from an IDL callback?
                        auto& callback = custom_element_upgrade_reaction.custom_element_definition->constructor();
                        auto& realm = callback.callback->shape().realm();
                        auto& global = realm.global_object();

                        auto& window_or_worker = as<HTML::WindowOrWorkerGlobalScopeMixin>(global);
                        window_or_worker.report_an_exception(maybe_exception.error_value());
                    }
                },
                [&](DOM::CustomElementCallbackReaction& custom_element_callback_reaction) -> void {
                    // -> callback reaction
                    //      Invoke reaction's callback function with reaction's arguments and "report", and callback this value set to element.
                    (void)WebIDL::invoke_callback(*custom_element_callback_reaction.callback, element.ptr(), WebIDL::ExceptionBehavior::Report, custom_element_callback_reaction.arguments);
                });
        }
    }
}

}
