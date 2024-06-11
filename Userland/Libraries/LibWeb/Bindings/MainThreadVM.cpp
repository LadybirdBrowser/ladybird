/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022-2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Heap/DeferGC.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/FinalizationRegistry.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/SourceTextModule.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/WindowExposedInterfaces.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/MutationType.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/PromiseRejectionEvent.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/Script.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/MathML/TagNames.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/NavigationTiming/EntryNames.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/XHR/EventNames.h>
#include <LibWeb/XLink/AttributeNames.h>

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
        [](JS::NonnullGCPtr<JS::Script>& js_script) -> HTML::Script* {
            return verify_cast<HTML::ClassicScript>(js_script->host_defined());
        },
        [](JS::NonnullGCPtr<JS::Module>& js_module) -> HTML::Script* {
            return verify_cast<HTML::ModuleScript>(js_module->host_defined());
        },
        [](Empty) -> HTML::Script* {
            return nullptr;
        });
}

ErrorOr<void> initialize_main_thread_vm()
{
    VERIFY(!s_main_thread_vm);

    s_main_thread_vm = TRY(JS::VM::create(make<WebEngineCustomData>()));
    s_main_thread_vm->on_unimplemented_property_access = [](auto const& object, auto const& property_key) {
        dbgln("FIXME: Unimplemented IDL interface: '{}.{}'", object.class_name(), property_key.to_string());
    };

    // NOTE: We intentionally leak the main thread JavaScript VM.
    //       This avoids doing an exhaustive garbage collection on process exit.
    s_main_thread_vm->ref();

    auto& custom_data = verify_cast<WebEngineCustomData>(*s_main_thread_vm->custom_data());
    custom_data.event_loop = s_main_thread_vm->heap().allocate_without_realm<HTML::EventLoop>();

    // These strings could potentially live on the VM similar to CommonPropertyNames.
    DOM::MutationType::initialize_strings();
    HTML::AttributeNames::initialize_strings();
    HTML::CustomElementReactionNames::initialize_strings();
    HTML::EventNames::initialize_strings();
    HTML::TagNames::initialize_strings();
    MathML::TagNames::initialize_strings();
    Namespace::initialize_strings();
    NavigationTiming::EntryNames::initialize_strings();
    PerformanceTimeline::EntryTypes::initialize_strings();
    SVG::AttributeNames::initialize_strings();
    SVG::TagNames::initialize_strings();
    UIEvents::EventNames::initialize_strings();
    WebGL::EventNames::initialize_strings();
    XHR::EventNames::initialize_strings();
    XLink::AttributeNames::initialize_strings();

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
    s_main_thread_vm->host_promise_rejection_tracker = [](JS::Promise& promise, JS::Promise::RejectionOperation operation) {
        // 1. Let script be the running script.
        //    The running script is the script in the [[HostDefined]] field in the ScriptOrModule component of the running JavaScript execution context.
        HTML::Script* script { nullptr };
        s_main_thread_vm->running_execution_context().script_or_module.visit(
            [&script](JS::NonnullGCPtr<JS::Script>& js_script) {
                script = verify_cast<HTML::ClassicScript>(js_script->host_defined());
            },
            [&script](JS::NonnullGCPtr<JS::Module>& js_module) {
                script = verify_cast<HTML::ModuleScript>(js_module->host_defined());
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

        // 3. Let settings object be the current settings object.
        // 4. If script is not null, then set settings object to script's settings object.
        auto& settings_object = script ? script->settings_object() : HTML::current_settings_object();

        switch (operation) {
        case JS::Promise::RejectionOperation::Reject:
            // 4. If operation is "reject",
            //    1. Add promise to settings object's about-to-be-notified rejected promises list.
            settings_object.push_onto_about_to_be_notified_rejected_promises_list(promise);
            break;
        case JS::Promise::RejectionOperation::Handle: {
            // 5. If operation is "handle",
            //    1. If settings object's about-to-be-notified rejected promises list contains promise, then remove promise from that list and return.
            bool removed_about_to_be_notified_rejected_promise = settings_object.remove_from_about_to_be_notified_rejected_promises_list(promise);
            if (removed_about_to_be_notified_rejected_promise)
                return;

            // 3. Remove promise from settings object's outstanding rejected promises weak set.
            bool removed_outstanding_rejected_promise = settings_object.remove_from_outstanding_rejected_promises_weak_set(&promise);

            // 2. If settings object's outstanding rejected promises weak set does not contain promise, then return.
            // NOTE: This is done out of order because removed_outstanding_rejected_promise will be false if the promise wasn't in the set or true if it was and got removed.
            if (!removed_outstanding_rejected_promise)
                return;

            // 4. Let global be settings object's global object.
            auto& global = settings_object.global_object();

            // 5. Queue a global task on the DOM manipulation task source given global to fire an event named rejectionhandled at global, using PromiseRejectionEvent,
            //    with the promise attribute initialized to promise, and the reason attribute initialized to the value of promise's [[PromiseResult]] internal slot.
            HTML::queue_global_task(HTML::Task::Source::DOMManipulation, global, JS::create_heap_function(s_main_thread_vm->heap(), [&global, &promise] {
                // FIXME: This currently assumes that global is a WindowObject.
                auto& window = verify_cast<HTML::Window>(global);

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
    s_main_thread_vm->host_call_job_callback = [](JS::JobCallback& callback, JS::Value this_value, ReadonlySpan<JS::Value> arguments_list) {
        auto& callback_host_defined = verify_cast<WebEngineCustomJobCallbackData>(*callback.custom_data());

        // 1. Let incumbent settings be callback.[[HostDefined]].[[IncumbentSettings]]. (NOTE: Not necessary)
        // 2. Let script execution context be callback.[[HostDefined]].[[ActiveScriptContext]]. (NOTE: Not necessary)

        // 3. Prepare to run a callback with incumbent settings.
        callback_host_defined.incumbent_settings->prepare_to_run_callback();

        // 4. If script execution context is not null, then push script execution context onto the JavaScript execution context stack.
        if (callback_host_defined.active_script_context)
            s_main_thread_vm->push_execution_context(*callback_host_defined.active_script_context);

        // 5. Let result be Call(callback.[[Callback]], V, argumentsList).
        auto result = JS::call(*s_main_thread_vm, callback.callback(), this_value, arguments_list);

        // 6. If script execution context is not null, then pop script execution context from the JavaScript execution context stack.
        if (callback_host_defined.active_script_context) {
            VERIFY(&s_main_thread_vm->running_execution_context() == callback_host_defined.active_script_context.ptr());
            s_main_thread_vm->pop_execution_context();
        }

        // 7. Clean up after running a callback with incumbent settings.
        callback_host_defined.incumbent_settings->clean_up_after_running_callback();

        // 8. Return result.
        return result;
    };

    // 8.1.5.4.2 HostEnqueueFinalizationRegistryCleanupJob(finalizationRegistry), https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuefinalizationregistrycleanupjob
    s_main_thread_vm->host_enqueue_finalization_registry_cleanup_job = [](JS::FinalizationRegistry& finalization_registry) {
        // 1. Let global be finalizationRegistry.[[Realm]]'s global object.
        auto& global = finalization_registry.realm().global_object();

        // 2. Queue a global task on the JavaScript engine task source given global to perform the following steps:
        HTML::queue_global_task(HTML::Task::Source::JavaScriptEngine, global, JS::create_heap_function(s_main_thread_vm->heap(), [&finalization_registry] {
            // 1. Let entry be finalizationRegistry.[[CleanupCallback]].[[Callback]].[[Realm]]'s environment settings object.
            auto& entry = host_defined_environment_settings_object(*finalization_registry.cleanup_callback().callback().realm());

            // 2. Check if we can run script with entry. If this returns "do not run", then return.
            if (entry.can_run_script() == HTML::RunScriptDecision::DoNotRun)
                return;

            // 3. Prepare to run script with entry.
            entry.prepare_to_run_script();

            // 4. Let result be the result of performing CleanupFinalizationRegistry(finalizationRegistry).
            auto result = finalization_registry.cleanup();

            // 5. Clean up after running script with entry.
            entry.clean_up_after_running_script();

            // 6. If result is an abrupt completion, then report the exception given by result.[[Value]].
            if (result.is_error())
                HTML::report_exception(result, finalization_registry.realm());
        }));
    };

    // 8.1.5.4.3 HostEnqueuePromiseJob(job, realm), https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuepromisejob
    s_main_thread_vm->host_enqueue_promise_job = [](JS::NonnullGCPtr<JS::HeapFunction<JS::ThrowCompletionOr<JS::Value>()>> job, JS::Realm* realm) {
        // 1. If realm is not null, then let job settings be the settings object for realm. Otherwise, let job settings be null.
        HTML::EnvironmentSettingsObject* job_settings { nullptr };
        if (realm)
            job_settings = &host_defined_environment_settings_object(*realm);

        // IMPLEMENTATION DEFINED: The JS spec says we must take implementation defined steps to make the currently active script or module at the time of HostEnqueuePromiseJob being invoked
        //                         also be the active script or module of the job at the time of its invocation.
        //                         This means taking it here now and passing it through to the lambda.
        auto script_or_module = s_main_thread_vm->get_active_script_or_module();

        // 2. Queue a microtask on the surrounding agent's event loop to perform the following steps:
        // This instance of "queue a microtask" uses the "implied document". The best fit for "implied document" here is "If the task is being queued by or for a script, then return the script's settings object's responsible document."
        // Do note that "implied document" from the spec is handwavy and the spec authors are trying to get rid of it: https://github.com/whatwg/html/issues/4980
        auto* script = active_script();

        auto& heap = realm ? realm->heap() : s_main_thread_vm->heap();
        // NOTE: This keeps job_settings alive by keeping realm alive, which is holding onto job_settings.
        HTML::queue_a_microtask(script ? script->settings_object().responsible_document().ptr() : nullptr, JS::create_heap_function(heap, [job_settings, job = move(job), script_or_module = move(script_or_module)] {
            // The dummy execution context has to be kept up here to keep it alive for the duration of the function.
            OwnPtr<JS::ExecutionContext> dummy_execution_context;

            if (job_settings) {
                // 1. If job settings is not null, then check if we can run script with job settings. If this returns "do not run" then return.
                if (job_settings->can_run_script() == HTML::RunScriptDecision::DoNotRun)
                    return;

                // 2. If job settings is not null, then prepare to run script with job settings.
                job_settings->prepare_to_run_script();

                // IMPLEMENTATION DEFINED: Additionally to preparing to run a script, we also prepare to run a callback here. This matches WebIDL's
                //                         invoke_callback() / call_user_object_operation() functions, and prevents a crash in host_make_job_callback()
                //                         when getting the incumbent settings object.
                job_settings->prepare_to_run_callback();

                // IMPLEMENTATION DEFINED: Per the previous "implementation defined" comment, we must now make the script or module the active script or module.
                //                         Since the only active execution context currently is the realm execution context of job settings, lets attach it here.
                job_settings->realm_execution_context().script_or_module = script_or_module;
            } else {
                // FIXME: We need to setup a dummy execution context in case a JS::NativeFunction is called when processing the job.
                //        This is because JS::NativeFunction::call excepts something to be on the execution context stack to be able to get the caller context to initialize the environment.
                //        Do note that the JS spec gives _no_ guarantee that the execution context stack has something on it if HostEnqueuePromiseJob was called with a null realm: https://tc39.es/ecma262/#job-preparedtoevaluatecode
                dummy_execution_context = JS::ExecutionContext::create();
                dummy_execution_context->script_or_module = script_or_module;
                s_main_thread_vm->push_execution_context(*dummy_execution_context);
            }

            // 3. Let result be job().
            auto result = job->function()();

            // 4. If job settings is not null, then clean up after running script with job settings.
            if (job_settings) {
                // IMPLEMENTATION DEFINED: Disassociate the realm execution context from the script or module.
                job_settings->realm_execution_context().script_or_module = Empty {};

                // IMPLEMENTATION DEFINED: See comment above, we need to clean up the non-standard prepare_to_run_callback() call.
                job_settings->clean_up_after_running_callback();

                job_settings->clean_up_after_running_script();
            } else {
                // Pop off the dummy execution context. See the above FIXME block about why this is done.
                s_main_thread_vm->pop_execution_context();
            }

            // 5. If result is an abrupt completion, then report the exception given by result.[[Value]].
            if (result.is_error())
                HTML::report_exception(result, job_settings->realm());
        }));
    };

    // 8.1.5.4.4 HostMakeJobCallback(callable), https://html.spec.whatwg.org/multipage/webappapis.html#hostmakejobcallback
    s_main_thread_vm->host_make_job_callback = [](JS::FunctionObject& callable) -> JS::NonnullGCPtr<JS::JobCallback> {
        // 1. Let incumbent settings be the incumbent settings object.
        auto& incumbent_settings = HTML::incumbent_settings_object();

        // 2. Let active script be the active script.
        auto* script = active_script();

        // 3. Let script execution context be null.
        OwnPtr<JS::ExecutionContext> script_execution_context;

        // 4. If active script is not null, set script execution context to a new JavaScript execution context, with its Function field set to null,
        //    its Realm field set to active script's settings object's Realm, and its ScriptOrModule set to active script's record.
        if (script) {
            script_execution_context = JS::ExecutionContext::create();
            script_execution_context->function = nullptr;
            script_execution_context->realm = &script->settings_object().realm();
            if (is<HTML::ClassicScript>(script)) {
                script_execution_context->script_or_module = JS::NonnullGCPtr<JS::Script>(*verify_cast<HTML::ClassicScript>(script)->script_record());
            } else if (is<HTML::ModuleScript>(script)) {
                if (is<HTML::JavaScriptModuleScript>(script)) {
                    script_execution_context->script_or_module = JS::NonnullGCPtr<JS::Module>(*verify_cast<HTML::JavaScriptModuleScript>(script)->record());
                } else {
                    // NOTE: Handle CSS and JSON module scripts once we have those.
                    VERIFY_NOT_REACHED();
                }
            } else {
                VERIFY_NOT_REACHED();
            }
        }

        // 5. Return the JobCallback Record { [[Callback]]: callable, [[HostDefined]]: { [[IncumbentSettings]]: incumbent settings, [[ActiveScriptContext]]: script execution context } }.
        auto host_defined = adopt_own(*new WebEngineCustomJobCallbackData(incumbent_settings, move(script_execution_context)));
        return JS::JobCallback::create(*s_main_thread_vm, callable, move(host_defined));
    };

    // 8.1.5.5.1 HostGetImportMetaProperties(moduleRecord), https://html.spec.whatwg.org/multipage/webappapis.html#hostgetimportmetaproperties
    s_main_thread_vm->host_get_import_meta_properties = [](JS::SourceTextModule& module_record) {
        auto& realm = module_record.realm();
        auto& vm = realm.vm();

        // 1. Let moduleScript be moduleRecord.[[HostDefined]].
        auto& module_script = *verify_cast<HTML::Script>(module_record.host_defined());

        // 2. Assert: moduleScript's base URL is not null, as moduleScript is a JavaScript module script.
        VERIFY(module_script.base_url().is_valid());

        // 3. Let urlString be moduleScript's base URL, serialized.
        auto url_string = module_script.base_url().serialize();

        // 4. Let steps be the following steps, given the argument specifier:
        auto steps = [module_script = JS::NonnullGCPtr { module_script }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto specifier = vm.argument(0);

            // 1. Set specifier to ? ToString(specifier).
            auto specifier_string = TRY(specifier.to_string(vm));

            // 2. Let url be the result of resolving a module specifier given moduleScript and specifier.
            auto url = TRY(Bindings::throw_dom_exception_if_needed(vm, [&] {
                return HTML::resolve_module_specifier(*module_script, specifier_string.to_byte_string());
            }));

            // 3. Return the serialization of url.
            return JS::PrimitiveString::create(vm, url.serialize());
        };

        // 4. Let resolveFunction be ! CreateBuiltinFunction(steps, 1, "resolve", « »).
        auto resolve_function = JS::NativeFunction::create(realm, move(steps), 1, vm.names.resolve);

        // 5. Return « Record { [[Key]]: "url", [[Value]]: urlString }, Record { [[Key]]: "resolve", [[Value]]: resolveFunction } ».
        HashMap<JS::PropertyKey, JS::Value> meta;
        meta.set("url", JS::PrimitiveString::create(vm, move(url_string)));
        meta.set("resolve", resolve_function);

        return meta;
    };

    // FIXME: Implement 8.1.5.5.2 HostImportModuleDynamically(referencingScriptOrModule, moduleRequest, promiseCapability), https://html.spec.whatwg.org/multipage/webappapis.html#hostimportmoduledynamically(referencingscriptormodule,-modulerequest,-promisecapability)
    // FIXME: Implement 8.1.5.5.3 HostResolveImportedModule(referencingScriptOrModule, moduleRequest), https://html.spec.whatwg.org/multipage/webappapis.html#hostresolveimportedmodule(referencingscriptormodule,-modulerequest)

    // 8.1.6.5.2 HostGetSupportedImportAttributes(), https://html.spec.whatwg.org/multipage/webappapis.html#hostgetsupportedimportassertions
    s_main_thread_vm->host_get_supported_import_attributes = []() -> Vector<ByteString> {
        // 1. Return « "type" ».
        return { "type"sv };
    };

    // 8.1.6.5.3 HostLoadImportedModule(referrer, moduleRequest, loadState, payload), https://html.spec.whatwg.org/multipage/webappapis.html#hostloadimportedmodule
    s_main_thread_vm->host_load_imported_module = [](JS::ImportedModuleReferrer referrer, JS::ModuleRequest const& module_request, JS::GCPtr<JS::GraphLoadingState::HostDefined> load_state, JS::ImportedModulePayload payload) -> void {
        auto& vm = *s_main_thread_vm;
        auto& realm = *vm.current_realm();

        // 1. Let settingsObject be the current settings object.
        Optional<HTML::EnvironmentSettingsObject&> settings_object = HTML::current_settings_object();

        // FIXME: 2. If settingsObject's global object implements WorkletGlobalScope or ServiceWorkerGlobalScope and loadState is undefined, then:

        // 3. Let referencingScript be null.
        Optional<HTML::Script&> referencing_script;

        // FIXME: 4. Let fetchOptions be the default classic script fetch options.
        auto fetch_options = HTML::default_classic_script_fetch_options();

        // 5. Let fetchReferrer be "client".
        auto fetch_referrer = Fetch::Infrastructure::Request::Referrer::Client;

        // 6. If referrer is a Script Record or a Module Record, then:
        if (referrer.has<JS::NonnullGCPtr<JS::Script>>() || referrer.has<JS::NonnullGCPtr<JS::CyclicModule>>()) {
            // 1. Set referencingScript to referrer.[[HostDefined]].
            referencing_script = verify_cast<HTML::Script>(referrer.has<JS::NonnullGCPtr<JS::Script>>() ? *referrer.get<JS::NonnullGCPtr<JS::Script>>()->host_defined() : *referrer.get<JS::NonnullGCPtr<JS::CyclicModule>>()->host_defined());

            // 2. Set settingsObject to referencingScript's settings object.
            settings_object = referencing_script->settings_object();

            // FIXME: 3. Set fetchOptions to the new descendant script fetch options for referencingScript's fetch options.

            // FIXME: 4. Assert: fetchOptions is not null, as referencingScript is a classic script or a JavaScript module script.

            // FIXME: 5. Set fetchReferrer to referrer's base URL.
        }

        // 7. Disallow further import maps given settingsObject.
        settings_object->disallow_further_import_maps();

        // 8. Let url be the result of resolving a module specifier given referencingScript and moduleRequest.[[Specifier]],
        //    catching any exceptions. If they throw an exception, let resolutionError be the thrown exception.
        auto url = HTML::resolve_module_specifier(referencing_script, module_request.module_specifier);

        // 9. If the previous step threw an exception, then:
        if (url.is_exception()) {
            // 1. Let completion be Completion Record { [[Type]]: throw, [[Value]]: resolutionError, [[Target]]: empty }.
            auto completion = dom_exception_to_throw_completion(main_thread_vm(), url.exception());

            // 2. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, completion).
            HTML::TemporaryExecutionContext context { host_defined_environment_settings_object(realm) };
            JS::finish_loading_imported_module(referrer, module_request, payload, completion);

            // 3. Return.
            return;
        }

        // 10. Let destination be "script".
        auto destination = Fetch::Infrastructure::Request::Destination::Script;

        // 11. Let fetchClient be settingsObject.
        JS::NonnullGCPtr fetch_client { *settings_object };

        // 12. If loadState is not undefined, then:
        HTML::PerformTheFetchHook perform_fetch;
        if (load_state) {
            auto& fetch_context = static_cast<HTML::FetchContext&>(*load_state);

            // 1. Set destination to loadState.[[Destination]].
            destination = fetch_context.destination;

            // 2. Set fetchClient loadState.[[FetchClient]].
            fetch_client = fetch_context.fetch_client;

            // For step 13
            perform_fetch = fetch_context.perform_fetch;
        }

        auto on_single_fetch_complete = HTML::create_on_fetch_script_complete(realm.heap(), [referrer, &realm, load_state, module_request, payload](JS::GCPtr<HTML::Script> const& module_script) -> void {
            // onSingleFetchComplete given moduleScript is the following algorithm:
            // 1. Let completion be null.
            // NOTE: Our JS::Completion does not support non JS::Value types for its [[Value]], a such we
            //       use JS::ThrowCompletionOr here.

            auto& vm = realm.vm();
            JS::GCPtr<JS::Module> module = nullptr;

            auto completion = [&]() -> JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Module>> {
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
                // 4. Otherwise, set completion to Completion Record { [[Type]]: normal, [[Value]]: result's record, [[Target]]: empty }.
                else {
                    module = static_cast<HTML::JavaScriptModuleScript&>(*module_script).record();
                    return JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Module>>(*module);
                }
            }();

            // 5. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, completion).
            // NON-STANDARD: To ensure that LibJS can find the module on the stack, we push a new execution context.

            auto module_execution_context = JS::ExecutionContext::create();
            module_execution_context->realm = realm;
            if (module)
                module_execution_context->script_or_module = JS::NonnullGCPtr { *module };
            vm.push_execution_context(*module_execution_context);

            JS::finish_loading_imported_module(referrer, module_request, payload, completion);

            vm.pop_execution_context();
        });

        // 13. Fetch a single imported module script given url, fetchClient, destination, fetchOptions, settingsObject, fetchReferrer,
        //     moduleRequest, and onSingleFetchComplete as defined below.
        //     If loadState is not undefined and loadState.[[PerformFetch]] is not null, pass loadState.[[PerformFetch]] along as well.
        HTML::fetch_single_imported_module_script(realm, url.release_value(), *fetch_client, destination, fetch_options, *settings_object, fetch_referrer, module_request, perform_fetch, on_single_fetch_complete);
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
    auto& custom_data = verify_cast<WebEngineCustomData>(*vm.custom_data());

    // 1. If the surrounding agent’s mutation observer microtask queued is true, then return.
    if (custom_data.mutation_observer_microtask_queued)
        return;

    // 2. Set the surrounding agent’s mutation observer microtask queued to true.
    custom_data.mutation_observer_microtask_queued = true;

    // 3. Queue a microtask to notify mutation observers.
    // NOTE: This uses the implied document concept. In the case of mutation observers, it is always done in a node context, so document should be that node's document.
    // FIXME: Is it safe to pass custom_data through?
    HTML::queue_a_microtask(&document, JS::create_heap_function(vm.heap(), [&custom_data, &heap = document.heap()]() {
        // 1. Set the surrounding agent’s mutation observer microtask queued to false.
        custom_data.mutation_observer_microtask_queued = false;

        // 2. Let notifySet be a clone of the surrounding agent’s mutation observers.
        JS::MarkedVector<DOM::MutationObserver*> notify_set(heap);
        for (auto& observer : custom_data.mutation_observers)
            notify_set.append(observer);

        // FIXME: 3. Let signalSet be a clone of the surrounding agent’s signal slots.

        // FIXME: 4. Empty the surrounding agent’s signal slots.

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

            // 4. If records is not empty, then invoke mo’s callback with « records, mo », and mo. If this throws an exception, catch it, and report the exception.
            if (!records.is_empty()) {
                auto& callback = mutation_observer->callback();
                auto& realm = callback.callback_context->realm();

                auto wrapped_records = MUST(JS::Array::create(realm, 0));
                for (size_t i = 0; i < records.size(); ++i) {
                    auto& record = records.at(i);
                    auto property_index = JS::PropertyKey { i };
                    MUST(wrapped_records->create_data_property(property_index, record.ptr()));
                }

                auto result = WebIDL::invoke_callback(callback, mutation_observer, wrapped_records, mutation_observer);
                if (result.is_abrupt())
                    HTML::report_exception(result, realm);
            }
        }

        // FIXME: 6. For each slot of signalSet, fire an event named slotchange, with its bubbles attribute set to true, at slot.
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

void WebEngineCustomData::spin_event_loop_until(JS::SafeFunction<bool()> goal_condition)
{
    Platform::EventLoopPlugin::the().spin_until(move(goal_condition));
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#invoke-custom-element-reactions
void invoke_custom_element_reactions(Vector<JS::Handle<DOM::Element>>& element_queue)
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

            auto maybe_exception = reaction.visit(
                [&](DOM::CustomElementUpgradeReaction const& custom_element_upgrade_reaction) -> JS::ThrowCompletionOr<void> {
                    // -> upgrade reaction
                    //      Upgrade element using reaction's custom element definition.
                    return element->upgrade_element(*custom_element_upgrade_reaction.custom_element_definition);
                },
                [&](DOM::CustomElementCallbackReaction& custom_element_callback_reaction) -> JS::ThrowCompletionOr<void> {
                    // -> callback reaction
                    //      Invoke reaction's callback function with reaction's arguments, and with element as the callback this value.
                    auto result = WebIDL::invoke_callback(*custom_element_callback_reaction.callback, element.ptr(), custom_element_callback_reaction.arguments);
                    if (result.is_abrupt())
                        return result.release_error();
                    return {};
                });

            // If this throws an exception, catch it, and report the exception.
            if (maybe_exception.is_throw_completion())
                HTML::report_exception(maybe_exception, element->realm());
        }
    }
}

}
