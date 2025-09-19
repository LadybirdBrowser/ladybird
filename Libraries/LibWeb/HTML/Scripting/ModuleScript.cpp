/*
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ModuleRequest.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

ModuleScript::~ModuleScript() = default;

ModuleScript::ModuleScript(Optional<URL::URL> base_url, ByteString filename, JS::Realm& realm)
    : Script(move(base_url), move(filename), realm)
{
}

// https://html.spec.whatwg.org/multipage/webappapis.html#creating-a-javascript-module-script
// https://whatpr.org/html/9893/webappapis.html#creating-a-javascript-module-script
WebIDL::ExceptionOr<GC::Ptr<ModuleScript>> ModuleScript::create_a_javascript_module_script(ByteString const& filename, StringView source, JS::Realm& realm, URL::URL base_url)
{
    // 1. If scripting is disabled for realm, then set source to the empty string.
    if (HTML::is_scripting_disabled(realm))
        source = ""sv;

    // 2. Let script be a new module script that this algorithm will subsequently initialize.
    // 3. Set script's realm to realm.
    // 4. Set script's base URL to baseURL.
    auto script = realm.create<ModuleScript>(move(base_url), filename, realm);

    // FIXME: 5. Set script's fetch options to options.

    // 6. Set script's parse error and error to rethrow to null.
    script->set_parse_error(JS::js_null());
    script->set_error_to_rethrow(JS::js_null());

    // 7. Let result be ParseModule(source, realm, script).
    auto result = JS::SourceTextModule::parse(source, realm, filename.view(), script);

    // 8. If result is a list of errors, then:
    if (result.is_error()) {
        auto& parse_error = result.error().first();
        dbgln("JavaScriptModuleScript: Failed to parse: {}", parse_error.to_string());

        // 1. Set script's parse error to result[0].
        script->set_parse_error(JS::SyntaxError::create(realm, parse_error.to_string()));

        // 2. Return script.
        return script;
    }

    // 9. Set script's record to result.
    script->m_record = result.value();

    // 10. Return script.
    return script;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#creating-a-css-module-script
// https://whatpr.org/html/9893/webappapis.html#creating-a-css-module-script
WebIDL::ExceptionOr<GC::Ptr<ModuleScript>> ModuleScript::create_a_css_module_script(ByteString const& filename, StringView source, JS::Realm& realm)
{
    // 1. Let script be a new module script that this algorithm will subsequently initialize.
    // 2. Set script's realm to realm.
    // 3. Set script's base URL and fetch options to null.
    auto script = realm.create<ModuleScript>(Optional<URL::URL> {}, filename, realm);

    // 4. Set script's parse error and error to rethrow to null.
    script->set_parse_error(JS::js_null());
    script->set_error_to_rethrow(JS::js_null());

    // 5. Let sheet be the result of running the steps to create a constructed CSSStyleSheet with an empty dictionary as
    //    the argument.
    auto sheet = TRY(CSS::CSSStyleSheet::construct_impl(realm));

    // 6. Run the steps to synchronously replace the rules of a CSSStyleSheet on sheet given source.
    //    If this throws an exception, catch it, and set script's parse error to that exception, and return script.
    if (auto result = sheet->replace_sync(source); result.is_error()) {
        auto throw_completion = Bindings::exception_to_throw_completion(realm.vm(), result.exception());
        script->set_parse_error(throw_completion.value());
        return script;
    }

    // 7. Set script's record to the result of CreateDefaultExportSyntheticModule(sheet).
    script->m_record = JS::SyntheticModule::create_default_export_synthetic_module(realm, sheet, filename.view());

    // 8. Return script.
    return script;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#creating-a-json-module-script
// https://whatpr.org/html/9893/webappapis.html#creating-a-json-module-script
WebIDL::ExceptionOr<GC::Ptr<ModuleScript>> ModuleScript::create_a_json_module_script(ByteString const& filename, StringView source, JS::Realm& realm)
{
    // 1. Let script be a new module script that this algorithm will subsequently initialize.
    // 2. Set script's realm to realm.
    // 3. Set script's base URL and fetch options to null.
    //    FIXME: Set options.
    auto script = realm.create<ModuleScript>(Optional<URL::URL> {}, filename, realm);

    // 4. Set script's parse error and error to rethrow to null.
    script->set_parse_error(JS::js_null());
    script->set_error_to_rethrow(JS::js_null());

    // 5. Let result be ParseJSONModule(source).
    //    If this throws an exception, catch it, and set script's parse error to that exception, and return script.
    TemporaryExecutionContext execution_context { realm };
    auto result = JS::parse_json_module(realm, source, filename);
    if (result.is_error()) {
        script->set_parse_error(result.error().value());
        return script;
    }

    // 6. Set script's record to result.
    script->m_record = result.value();

    // 7. Return script.
    return script;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#creating-a-webassembly-module-script
// https://whatpr.org/html/9893/webappapis.html#creating-a-webassembly-module-script
WebIDL::ExceptionOr<GC::Ptr<ModuleScript>> ModuleScript::create_a_webassembly_module_script(ByteString const& filename, ByteBuffer body_bytes, JS::Realm& realm, URL::URL base_url)
{
    // 1. If scripting is disabled for realm, then set bodyBytes to the byte sequence 0x00 0x61 0x73 0x6d 0x01 0x00 0x00 0x00.
    // NOTE: This byte sequence corresponds to an empty WebAssembly module with only the magic bytes and version number provided.
    if (HTML::is_scripting_disabled(realm)) {
        auto byte_sequence = ReadonlyBytes { { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 } };
        body_bytes = MUST(ByteBuffer::create_uninitialized(byte_sequence.size()));
        byte_sequence.copy_to(body_bytes);
    }

    // 2. Let script be a new module script that this algorithm will subsequently initialize.
    // 3. Set script's realm to realm.
    // 4. Set script's base URL to baseURL.
    // FIXME: 5. Set script's fetch options to options.
    auto script = realm.create<ModuleScript>(base_url, filename, realm);

    // 6. Set script's parse error and error to rethrow to null.
    script->set_parse_error(JS::js_null());
    script->set_error_to_rethrow(JS::js_null());

    // 7. Let result be the result of parsing a web assembly module given bodyBytes, realm, and script.
    // NOTE: Passing script as the last parameter here ensures result.[[HostDefined]] will be script.
    TemporaryExecutionContext execution_context { realm };
    auto result = WebAssembly::WebAssemblyModule::parse(body_bytes, realm, filename, script);

    // 8. If the previous step threw an error error, then:
    if (result.is_error()) {
        // 1. Set script's parse error to error.
        script->set_parse_error(result.error().value());

        // 2. Return script.
        return script;
    }

    // 9. Set script's record to result.
    script->m_record = result.value();

    // 10. Return script.
    return script;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#run-a-module-script
// https://whatpr.org/html/9893/webappapis.html#run-a-module-script
WebIDL::Promise* ModuleScript::run(PreventErrorReporting prevent_error_reporting)
{
    // 1. Let realm be the realm of script.
    auto& realm = this->realm();

    // 2. Check if we can run script with realm. If this returns "do not run", then return a promise resolved with undefined.
    if (can_run_script(realm) == RunScriptDecision::DoNotRun) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    }

    // FIXME: 3. Record module script execution start time given script.

    // 4. Prepare to run script given realm.
    prepare_to_run_script(realm);

    // 5. Let evaluationPromise be null.
    GC::Ptr<WebIDL::Promise> evaluation_promise = nullptr;

    // 6. If script's error to rethrow is not null, then set evaluationPromise to a promise rejected with script's error to rethrow.
    if (!error_to_rethrow().is_null()) {
        evaluation_promise = WebIDL::create_rejected_promise(realm, error_to_rethrow());
    }
    // 7. Otherwise:
    else {
        // 1. Let record be script's record.
        auto record = m_record.visit(
            [](Empty) -> GC::Ref<JS::Module> { VERIFY_NOT_REACHED(); },
            [](auto& module) -> GC::Ref<JS::Module> { return module; });

        // NON-STANDARD: To ensure that LibJS can find the module on the stack, we push a new execution context.
        JS::ExecutionContext* module_execution_context = nullptr;
        ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(module_execution_context, 0, 0);
        module_execution_context->realm = &realm;
        module_execution_context->script_or_module = record;
        vm().push_execution_context(*module_execution_context);

        // 2. Set evaluationPromise to record.Evaluate().
        auto elevation_promise_or_error = record->evaluate(vm());

        // NOTE: This step will recursively evaluate all of the module's dependencies.
        // If Evaluate fails to complete as a result of the user agent aborting the running script,
        // then set evaluationPromise to a promise rejected with a new "QuotaExceededError" DOMException.
        if (elevation_promise_or_error.is_error()) {
            evaluation_promise = WebIDL::create_rejected_promise(realm, WebIDL::QuotaExceededError::create(realm, "Failed to evaluate module script"_utf16).ptr());
        } else {
            evaluation_promise = elevation_promise_or_error.value();
        }

        // NON-STANDARD: Pop the execution context mentioned above.
        vm().pop_execution_context();
    }

    // 8. If preventErrorReporting is false, then upon rejection of evaluationPromise with reason, report the exception given by reason for script.
    if (prevent_error_reporting == PreventErrorReporting::No) {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        evaluation_promise = WebIDL::upon_rejection(*evaluation_promise, GC::create_function(realm.heap(), [&realm](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            auto& window_or_worker = as<WindowOrWorkerGlobalScopeMixin>(realm.global_object());
            window_or_worker.report_an_exception(reason);
            return throw_completion(reason);
        }));
    }

    // 9. Clean up after running script with realm.
    clean_up_after_running_script(realm);

    // 10. Return evaluationPromise.
    return evaluation_promise;
}

void ModuleScript::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_record.visit(
        [&](Empty) {},
        [&](auto record) { visitor.visit(record); });
}

}
