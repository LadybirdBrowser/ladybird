/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/ElapsedTimer.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ClassicScript);

// https://html.spec.whatwg.org/multipage/webappapis.html#creating-a-classic-script
// https://whatpr.org/html/9893/webappapis.html#creating-a-classic-script
GC::Ref<ClassicScript> ClassicScript::create(ByteString filename, StringView source, JS::Realm& realm, URL::URL base_url, size_t source_line_number, MutedErrors muted_errors)
{
    auto& vm = realm.vm();

    // 1. If muted errors is true, then set baseURL to about:blank.
    if (muted_errors == MutedErrors::Yes)
        base_url = "about:blank"sv;

    // 2. If scripting is disabled for realm, then set source to the empty string.
    if (is_scripting_disabled(realm))
        source = ""sv;

    // 3. Let script be a new classic script that this algorithm will subsequently initialize.
    // 4. Set script's realm to realm.
    // 5. Set script's base URL to baseURL.
    auto script = vm.heap().allocate<ClassicScript>(move(base_url), move(filename), realm);

    // FIXME: 6. Set script's fetch options to options.

    // 7. Set script's muted errors to muted errors.
    script->m_muted_errors = muted_errors;

    // 8. Set script's parse error and error to rethrow to null.
    script->set_parse_error(JS::js_null());
    script->set_error_to_rethrow(JS::js_null());

    // FIXME: 9. Record classic script creation time given script and sourceURLForWindowScripts .

    // 10. Let result be ParseScript(source, realm, script).
    auto parse_timer = Core::ElapsedTimer::start_new();
    auto result = JS::Script::parse(source, realm, script->filename(), script, source_line_number);
    dbgln_if(HTML_SCRIPT_DEBUG, "ClassicScript: Parsed {} in {}ms", script->filename(), parse_timer.elapsed_milliseconds());

    // 11. If result is a list of errors, then:
    if (result.is_error()) {
        auto& parse_error = result.error().first();
        dbgln_if(HTML_SCRIPT_DEBUG, "ClassicScript: Failed to parse: {}", parse_error.to_string());

        // 1. Set script's parse error and its error to rethrow to result[0].
        script->set_parse_error(JS::SyntaxError::create(realm, parse_error.to_string()));
        script->set_error_to_rethrow(script->parse_error());

        // 2. Return script.
        return script;
    }

    // 12. Set script's record to result.
    script->m_script_record = *result.release_value();

    // 13. Return script.
    return script;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#run-a-classic-script
// https://whatpr.org/html/9893/webappapis.html#run-a-classic-script
JS::Completion ClassicScript::run(RethrowErrors rethrow_errors, GC::Ptr<JS::Environment> lexical_environment_override)
{
    // 1. Let realm be the realm of script.
    auto& realm = this->realm();

    // 2. Check if we can run script with realm. If this returns "do not run" then return NormalCompletion(empty).
    if (can_run_script(realm) == RunScriptDecision::DoNotRun)
        return JS::normal_completion({});

    // 3. Prepare to run script given realm.
    prepare_to_run_script(realm);

    // 4. Let evaluationStatus be null.
    JS::Completion evaluation_status;

    // 5. If script's error to rethrow is not null, then set evaluationStatus to Completion { [[Type]]: throw, [[Value]]: script's error to rethrow, [[Target]]: empty }.
    if (!error_to_rethrow().is_null()) {
        evaluation_status = JS::Completion { JS::Completion::Type::Throw, error_to_rethrow() };
    } else {
        auto timer = Core::ElapsedTimer::start_new();

        // 6. Otherwise, set evaluationStatus to ScriptEvaluation(script's record).
        evaluation_status = vm().bytecode_interpreter().run(*m_script_record, lexical_environment_override);

        // FIXME: If ScriptEvaluation does not complete because the user agent has aborted the running script, leave evaluationStatus as null.

        dbgln_if(HTML_SCRIPT_DEBUG, "ClassicScript: Finished running script {}, Duration: {}ms", filename(), timer.elapsed_milliseconds());
    }

    // 7. If evaluationStatus is an abrupt completion, then:
    if (evaluation_status.is_abrupt()) {
        // 1. If rethrow errors is true and script's muted errors is false, then:
        if (rethrow_errors == RethrowErrors::Yes && m_muted_errors == MutedErrors::No) {
            // 1. Clean up after running script with realm.
            clean_up_after_running_script(realm);

            // 2. Rethrow evaluationStatus.[[Value]].
            return JS::throw_completion(*evaluation_status.value());
        }

        // 2. If rethrow errors is true and script's muted errors is true, then:
        if (rethrow_errors == RethrowErrors::Yes && m_muted_errors == MutedErrors::Yes) {
            // 1. Clean up after running script with realm.
            clean_up_after_running_script(realm);

            // 2. Throw a "NetworkError" DOMException.
            return throw_completion(WebIDL::NetworkError::create(realm, "Script error."_string));
        }

        // 3. Otherwise, rethrow errors is false. Perform the following steps:
        VERIFY(rethrow_errors == RethrowErrors::No);

        // 1. Report an exception given by evaluationStatus.[[Value]] for realms's global object.
        auto& window_or_worker = as<WindowOrWorkerGlobalScopeMixin>(realm.global_object());
        window_or_worker.report_an_exception(*evaluation_status.value());

        // 2. Clean up after running script with realm.
        clean_up_after_running_script(realm);

        // 3. Return evaluationStatus.
        return evaluation_status;
    }

    // 8. Clean up after running script with realm.
    clean_up_after_running_script(realm);

    // 9. If evaluationStatus is a normal completion, then return evaluationStatus.
    VERIFY(!evaluation_status.is_abrupt());
    return evaluation_status;

    // FIXME: 10. If we've reached this point, evaluationStatus was left as null because the script was aborted prematurely during evaluation.
    //            Return Completion { [[Type]]: throw, [[Value]]: a new "QuotaExceededError" DOMException, [[Target]]: empty }.
}

ClassicScript::ClassicScript(URL::URL base_url, ByteString filename, JS::Realm& realm)
    : Script(move(base_url), move(filename), realm)
{
}

ClassicScript::~ClassicScript() = default;

void ClassicScript::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_script_record);
}

}
