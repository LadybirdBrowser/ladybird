/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Lexer.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/ModuleNamespaceObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/ShadowRealm.h>
#include <LibJS/Runtime/WrappedFunction.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ShadowRealm);

ShadowRealm::ShadowRealm(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

void ShadowRealm::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_shadow_realm);
}

// 3.1.2 CopyNameAndLength ( F: a function object, Target: a function object, optional prefix: a String, optional argCount: a Number, ), https://tc39.es/proposal-shadowrealm/#sec-copynameandlength
ThrowCompletionOr<void> copy_name_and_length(VM& vm, FunctionObject& function, FunctionObject& target, Optional<StringView> prefix, Optional<unsigned> arg_count)
{
    // 1. If argCount is undefined, then set argCount to 0.
    if (!arg_count.has_value())
        arg_count = 0;

    // 2. Let L be 0.
    double length = 0;

    // 3. Let targetHasLength be ? HasOwnProperty(Target, "length").
    auto target_has_length = TRY(target.has_own_property(vm.names.length));

    // 4. If targetHasLength is true, then
    if (target_has_length) {
        // a. Let targetLen be ? Get(Target, "length").
        auto target_length = TRY(target.get(vm.names.length));

        // b. If Type(targetLen) is Number, then
        if (target_length.is_number()) {
            // i. If targetLen is +∞𝔽, set L to +∞.
            if (target_length.is_positive_infinity()) {
                length = target_length.as_double();
            }
            // ii. Else if targetLen is -∞𝔽, set L to 0.
            else if (target_length.is_negative_infinity()) {
                length = 0;
            }
            // iii. Else,
            else {
                // 1. Let targetLenAsInt be ! ToIntegerOrInfinity(targetLen).
                auto target_length_as_int = MUST(target_length.to_integer_or_infinity(vm));

                // 2. Assert: targetLenAsInt is finite.
                VERIFY(!isinf(target_length_as_int));

                // 3. Set L to max(targetLenAsInt - argCount, 0).
                length = max(target_length_as_int - *arg_count, 0);
            }
        }
    }

    // 5. Perform SetFunctionLength(F, L).
    function.set_function_length(length);

    // 6. Let targetName be ? Get(Target, "name").
    auto target_name = TRY(target.get(vm.names.name));

    // 7. If Type(targetName) is not String, set targetName to the empty String.
    if (!target_name.is_string())
        target_name = PrimitiveString::create(vm, String {});

    // 8. Perform SetFunctionName(F, targetName, prefix).
    function.set_function_name({ target_name.as_string().byte_string() }, move(prefix));

    return {};
}

// 3.1.3 PerformShadowRealmEval ( sourceText: a String, callerRealm: a Realm Record, evalRealm: a Realm Record, ), https://tc39.es/proposal-shadowrealm/#sec-performshadowrealmeval
ThrowCompletionOr<Value> perform_shadow_realm_eval(VM& vm, StringView source_text, Realm& caller_realm, Realm& eval_realm)
{
    // FIXME: Needs to be updated to latest ECMA-262. See: https://github.com/tc39/proposal-shadowrealm/issues/367
    // 1. Perform ? HostEnsureCanCompileStrings(callerRealm, evalRealm).
    TRY(vm.host_ensure_can_compile_strings(eval_realm));

    // 2. Perform the following substeps in an implementation-defined order, possibly interleaving parsing and error detection:

    // a. Let script be ParseText(StringToCodePoints(sourceText), Script).
    auto parser = Parser(Lexer(source_text), Program::Type::Script, Parser::EvalInitialState {});
    auto program = parser.parse_program();

    // b. If script is a List of errors, throw a SyntaxError exception.
    if (parser.has_errors()) {
        auto& error = parser.errors()[0];
        return vm.throw_completion<SyntaxError>(error.to_string());
    }

    // c. If script Contains ScriptBody is false, return undefined.
    if (program->children().is_empty())
        return js_undefined();

    // d. Let body be the ScriptBody of script.
    // e. If body Contains NewTarget is true, throw a SyntaxError exception.
    // f. If body Contains SuperProperty is true, throw a SyntaxError exception.
    // g. If body Contains SuperCall is true, throw a SyntaxError exception.
    // FIXME: Implement these, we probably need a generic way of scanning the AST for certain nodes.

    // 3. Let strictEval be IsStrict of script.
    auto strict_eval = program->is_strict_mode();

    // 4. Let runningContext be the running execution context.
    // NOTE: This would be unused due to step 11 and is omitted for that reason.

    // 5. Let lexEnv be NewDeclarativeEnvironment(evalRealm.[[GlobalEnv]]).
    Environment* lexical_environment = new_declarative_environment(eval_realm.global_environment()).ptr();

    // 6. Let varEnv be evalRealm.[[GlobalEnv]].
    Environment* variable_environment = &eval_realm.global_environment();

    // 7. If strictEval is true, set varEnv to lexEnv.
    if (strict_eval)
        variable_environment = lexical_environment;

    // 8. If runningContext is not already suspended, suspend runningContext.
    // NOTE: We don't support this concept yet.

    // 9. Let evalContext be a new ECMAScript code execution context.
    auto eval_context = ExecutionContext::create();

    // 10. Set evalContext's Function to null.
    eval_context->function = nullptr;

    // 11. Set evalContext's Realm to evalRealm.
    eval_context->realm = &eval_realm;

    // 12. Set evalContext's ScriptOrModule to null.
    // Note: This is already the default value.

    // 13. Set evalContext's VariableEnvironment to varEnv.
    eval_context->variable_environment = variable_environment;

    // 14. Set evalContext's LexicalEnvironment to lexEnv.
    eval_context->lexical_environment = lexical_environment;

    // Non-standard
    eval_context->is_strict_mode = strict_eval;

    // 15. Push evalContext onto the execution context stack; evalContext is now the running execution context.
    TRY(vm.push_execution_context(*eval_context, {}));

    // 16. Let result be Completion(EvalDeclarationInstantiation(body, varEnv, lexEnv, null, strictEval)).
    auto eval_result = eval_declaration_instantiation(vm, program, variable_environment, lexical_environment, nullptr, strict_eval);

    Completion result;

    // 17. If result.[[Type]] is normal, then
    if (!eval_result.is_throw_completion()) {
        // a. Set result to the result of evaluating body.
        auto maybe_executable = Bytecode::compile(vm, program, FunctionKind::Normal, "ShadowRealmEval"sv);
        if (maybe_executable.is_error())
            result = maybe_executable.release_error();
        else {
            auto executable = maybe_executable.release_value();

            auto result_and_return_register = vm.bytecode_interpreter().run_executable(*executable, {});
            if (result_and_return_register.value.is_error()) {
                result = result_and_return_register.value.release_error();
            } else {
                // Resulting value is in the accumulator.
                result = result_and_return_register.return_register_value.value_or(js_undefined());
            }
        }
    }

    // 18. If result.[[Type]] is normal and result.[[Value]] is empty, then
    if (result.type() == Completion::Type::Normal && !result.value().has_value()) {
        // a. Set result to NormalCompletion(undefined).
        result = normal_completion(js_undefined());
    }

    // 19. Suspend evalContext and remove it from the execution context stack.
    // NOTE: We don't support this concept yet.
    vm.pop_execution_context();

    // 20. Resume the context that is now on the top of the execution context stack as the running execution context.
    // NOTE: We don't support this concept yet.

    // 21. If result.[[Type]] is not normal, throw a TypeError exception.
    if (result.type() != Completion::Type::Normal)
        return vm.throw_completion<TypeError>(ErrorType::ShadowRealmEvaluateAbruptCompletion);

    // 22. Return ? GetWrappedValue(callerRealm, result.[[Value]]).
    return get_wrapped_value(vm, caller_realm, *result.value());

    // NOTE: Also see "Editor's Note" in the spec regarding the TypeError above.
}

// 3.1.4 ShadowRealmImportValue ( specifierString: a String, exportNameString: a String, callerRealm: a Realm Record, evalRealm: a Realm Record, evalContext: an execution context, ), https://tc39.es/proposal-shadowrealm/#sec-shadowrealmimportvalue
ThrowCompletionOr<Value> shadow_realm_import_value(VM& vm, ByteString specifier_string, ByteString export_name_string, Realm& caller_realm, Realm& eval_realm, ExecutionContext& eval_context)
{
    // FIXME: evalRealm isn't being used anywhere in this AO (spec issue)
    (void)eval_realm;

    auto& realm = *vm.current_realm();

    // 1. Assert: evalContext is an execution context associated to a ShadowRealm instance's [[ExecutionContext]].

    // 2. Let innerCapability be ! NewPromiseCapability(%Promise%).
    auto inner_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 3. Let runningContext be the running execution context.
    // 4. If runningContext is not already suspended, suspend runningContext.
    // NOTE: We don't support this concept yet.

    // 5. Push evalContext onto the execution context stack; evalContext is now the running execution context.
    TRY(vm.push_execution_context(eval_context, {}));

    // 6. Let referrer be the Realm component of evalContext.
    auto referrer = JS::NonnullGCPtr { *eval_context.realm };
    // 7. Perform HostLoadImportedModule(referrer, specifierString, empty, innerCapability).
    vm.host_load_imported_module(referrer, ModuleRequest { specifier_string }, nullptr, inner_capability);

    // 7. Suspend evalContext and remove it from the execution context stack.
    // NOTE: We don't support this concept yet.
    vm.pop_execution_context();

    // 8. Resume the context that is now on the top of the execution context stack as the running execution context.
    // NOTE: We don't support this concept yet.

    // 9. Let steps be the steps of an ExportGetter function as described below.
    auto steps = [string = move(export_name_string)](auto& vm) -> ThrowCompletionOr<Value> {
        // 1. Assert: exports is a module namespace exotic object.
        VERIFY(vm.argument(0).is_object());
        auto& exports = vm.argument(0).as_object();
        VERIFY(is<ModuleNamespaceObject>(exports));

        // 2. Let f be the active function object.
        auto function = vm.running_execution_context().function;

        // 3. Let string be f.[[ExportNameString]].
        // 4. Assert: Type(string) is String.

        // 5. Let hasOwn be ? HasOwnProperty(exports, string).
        auto has_own = TRY(exports.has_own_property(string));

        // 6. If hasOwn is false, throw a TypeError exception.
        if (!has_own)
            return vm.template throw_completion<TypeError>(ErrorType::MissingRequiredProperty, string);

        // 7. Let value be ? Get(exports, string).
        auto value = TRY(exports.get(string));

        // 8. Let realm be f.[[Realm]].
        auto* realm = function->realm();
        VERIFY(realm);

        // 9. Return ? GetWrappedValue(realm, value).
        return get_wrapped_value(vm, *realm, value);
    };

    // 10. Let onFulfilled be CreateBuiltinFunction(steps, 1, "", « [[ExportNameString]] », callerRealm).
    // 11. Set onFulfilled.[[ExportNameString]] to exportNameString.
    auto on_fulfilled = NativeFunction::create(realm, move(steps), 1, "", &caller_realm);

    // 12. Let promiseCapability be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // NOTE: Even though the spec tells us to use %ThrowTypeError%, it's not observable if we actually do.
    // Throw a nicer TypeError forwarding the import error message instead (we know the argument is an Error object).
    auto throw_type_error = NativeFunction::create(realm, {}, [](auto& vm) -> ThrowCompletionOr<Value> {
        return vm.template throw_completion<TypeError>(vm.argument(0).as_object().get_without_side_effects(vm.names.message).as_string().utf8_string());
    });

    // 13. Return PerformPromiseThen(innerCapability.[[Promise]], onFulfilled, callerRealm.[[Intrinsics]].[[%ThrowTypeError%]], promiseCapability).
    return verify_cast<Promise>(inner_capability->promise().ptr())->perform_then(on_fulfilled, throw_type_error, promise_capability);
}

// 3.1.5 GetWrappedValue ( callerRealm: a Realm Record, value: unknown, ), https://tc39.es/proposal-shadowrealm/#sec-getwrappedvalue
ThrowCompletionOr<Value> get_wrapped_value(VM& vm, Realm& caller_realm, Value value)
{
    auto& realm = *vm.current_realm();

    // 1. If Type(value) is Object, then
    if (value.is_object()) {
        // a. If IsCallable(value) is false, throw a TypeError exception.
        if (!value.is_function())
            return vm.throw_completion<TypeError>(ErrorType::ShadowRealmWrappedValueNonFunctionObject, value);

        // b. Return ? WrappedFunctionCreate(callerRealm, value).
        return TRY(WrappedFunction::create(realm, caller_realm, value.as_function()));
    }

    // 2. Return value.
    return value;
}

}
