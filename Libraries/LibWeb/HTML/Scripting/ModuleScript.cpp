/*
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ModuleRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(JavaScriptModuleScript);

ModuleScript::~ModuleScript() = default;

ModuleScript::ModuleScript(URL::URL base_url, ByteString filename, JS::Realm& realm)
    : Script(move(base_url), move(filename), realm)
{
}

JavaScriptModuleScript::~JavaScriptModuleScript() = default;

JavaScriptModuleScript::JavaScriptModuleScript(URL::URL base_url, ByteString filename, JS::Realm& realm)
    : ModuleScript(move(base_url), move(filename), realm)
{
}

// https://html.spec.whatwg.org/multipage/webappapis.html#creating-a-javascript-module-script
// https://whatpr.org/html/9893/webappapis.html#creating-a-javascript-module-script
WebIDL::ExceptionOr<GC::Ptr<JavaScriptModuleScript>> JavaScriptModuleScript::create(ByteString const& filename, StringView source, JS::Realm& realm, URL::URL base_url)
{
    // 1. If scripting is disabled for realm, then set source to the empty string.
    if (HTML::is_scripting_disabled(realm))
        source = ""sv;

    // 2. Let script be a new module script that this algorithm will subsequently initialize.
    // 3. Set script's realm to realm.
    // 4. Set script's base URL to baseURL.
    auto script = realm.create<JavaScriptModuleScript>(move(base_url), filename, realm);

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

// https://html.spec.whatwg.org/multipage/webappapis.html#run-a-module-script
// https://whatpr.org/html/9893/webappapis.html#run-a-module-script
JS::Promise* JavaScriptModuleScript::run(PreventErrorReporting)
{
    // 1. Let realm be the realm of script.
    auto& realm = this->realm();

    // 2. Check if we can run script with realm. If this returns "do not run", then return a promise resolved with undefined.
    if (can_run_script(realm) == RunScriptDecision::DoNotRun) {
        auto promise = JS::Promise::create(realm);
        promise->fulfill(JS::js_undefined());
        return promise;
    }

    // 3. Prepare to run script given realm.
    prepare_to_run_script(realm);

    // 4. Let evaluationPromise be null.
    JS::Promise* evaluation_promise = nullptr;

    // 5. If script's error to rethrow is not null, then set evaluationPromise to a promise rejected with script's error to rethrow.
    if (!error_to_rethrow().is_null()) {
        evaluation_promise = JS::Promise::create(realm);
        evaluation_promise->reject(error_to_rethrow());
    }
    // 6. Otherwise:
    else {
        // 1. Let record be script's record.
        auto record = m_record;
        VERIFY(record);

        // NON-STANDARD: To ensure that LibJS can find the module on the stack, we push a new execution context.
        auto module_execution_context = JS::ExecutionContext::create();
        module_execution_context->realm = &realm;
        module_execution_context->script_or_module = GC::Ref<JS::Module> { *record };
        vm().push_execution_context(*module_execution_context);

        // 2. Set evaluationPromise to record.Evaluate().
        auto elevation_promise_or_error = record->evaluate(vm());

        // NOTE: This step will recursively evaluate all of the module's dependencies.
        // If Evaluate fails to complete as a result of the user agent aborting the running script,
        // then set evaluationPromise to a promise rejected with a new "QuotaExceededError" DOMException.
        if (elevation_promise_or_error.is_error()) {
            auto promise = JS::Promise::create(realm);
            promise->reject(WebIDL::QuotaExceededError::create(realm, "Failed to evaluate module script"_string).ptr());

            evaluation_promise = promise;
        } else {
            evaluation_promise = elevation_promise_or_error.value();
        }

        // NON-STANDARD: Pop the execution context mentioned above.
        vm().pop_execution_context();
    }

    // FIXME: 7. If preventErrorReporting is false, then upon rejection of evaluationPromise with reason, report the exception given by reason for script.

    // 8. Clean up after running script with realm.
    clean_up_after_running_script(realm);

    // 9. Return evaluationPromise.
    return evaluation_promise;
}

void JavaScriptModuleScript::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_record);
}

}
