/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Lexer.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionConstructor.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GeneratorPrototype.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

JS_DEFINE_ALLOCATOR(FunctionConstructor);

FunctionConstructor::FunctionConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Function.as_string(), realm.intrinsics().function_prototype())
{
}

void FunctionConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 20.2.2.2 Function.prototype, https://tc39.es/ecma262/#sec-function.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().function_prototype(), 0);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 20.2.1.1.1 CreateDynamicFunction ( constructor, newTarget, kind, args ), https://tc39.es/ecma262/#sec-createdynamicfunction
ThrowCompletionOr<ECMAScriptFunctionObject*> FunctionConstructor::create_dynamic_function(VM& vm, FunctionObject& constructor, FunctionObject* new_target, FunctionKind kind, MarkedVector<Value> const& args)
{
    // 1. Let currentRealm be the current Realm Record.
    auto& current_realm = *vm.current_realm();

    // 2. Perform ? HostEnsureCanCompileStrings(currentRealm).
    TRY(vm.host_ensure_can_compile_strings(current_realm));

    // 3. If newTarget is undefined, set newTarget to constructor.
    if (new_target == nullptr)
        new_target = &constructor;

    StringView prefix;
    NonnullGCPtr<Object> (Intrinsics::*fallback_prototype)() = nullptr;

    switch (kind) {
    // 4. If kind is normal, then
    case FunctionKind::Normal:
        // a. Let prefix be "function".
        prefix = "function"sv;

        // b. Let exprSym be the grammar symbol FunctionExpression.
        // c. Let bodySym be the grammar symbol FunctionBody[~Yield, ~Await].
        // d. Let parameterSym be the grammar symbol FormalParameters[~Yield, ~Await].

        // e. Let fallbackProto be "%Function.prototype%".
        fallback_prototype = &Intrinsics::function_prototype;
        break;

    // 5. Else if kind is generator, then
    case FunctionKind::Generator:
        // a. Let prefix be "function*".
        prefix = "function*"sv;

        // b. Let exprSym be the grammar symbol GeneratorExpression.
        // c. Let bodySym be the grammar symbol GeneratorBody.
        // d. Let parameterSym be the grammar symbol FormalParameters[+Yield, ~Await].

        // e. Let fallbackProto be "%GeneratorFunction.prototype%".
        fallback_prototype = &Intrinsics::generator_function_prototype;
        break;

    // 6. Else if kind is async, then
    case FunctionKind::Async:
        // a. Let prefix be "async function".
        prefix = "async function"sv;

        // b. Let exprSym be the grammar symbol AsyncFunctionExpression.
        // c. Let bodySym be the grammar symbol AsyncFunctionBody.
        // d. Let parameterSym be the grammar symbol FormalParameters[~Yield, +Await].

        // e. Let fallbackProto be "%AsyncFunction.prototype%".
        fallback_prototype = &Intrinsics::async_function_prototype;
        break;

    // 7. Else,
    case FunctionKind::AsyncGenerator:
        // a. Assert: kind is asyncGenerator.

        // b. Let prefix be "async function*".
        prefix = "async function*"sv;

        // c. Let exprSym be the grammar symbol AsyncGeneratorExpression.
        // d. Let bodySym be the grammar symbol AsyncGeneratorBody.
        // e. Let parameterSym be the grammar symbol FormalParameters[+Yield, +Await].

        // f. Let fallbackProto be "%AsyncGeneratorFunction.prototype%".
        fallback_prototype = &Intrinsics::async_generator_function_prototype;
        break;

    default:
        VERIFY_NOT_REACHED();
    }

    // 8. Let argCount be the number of elements in args.
    auto arg_count = args.size();

    // 9. Let P be the empty String.
    ByteString parameters_string = "";

    Optional<Value> body_arg;

    // 10. If argCount = 0, let bodyArg be the empty String.
    if (arg_count == 0) {
        // Optimization: Instead of creating a PrimitiveString here, we just check if body_arg is empty in step 16.
    }
    // 11. Else if argCount = 1, let bodyArg be args[0].
    else if (arg_count == 1) {
        body_arg = args[0];
    }
    // 12. Else,
    else {
        // a. Assert: argCount > 1.
        VERIFY(arg_count > 1);

        // b. Let firstArg be args[0].
        // c. Set P to ? ToString(firstArg).
        // NOTE: Also done in the loop. We start at 0 instead and then join() with a comma.

        // d. Let k be 1.
        size_t k = 0;

        // e. Repeat, while k < argCount - 1,
        Vector<ByteString> parameters;
        for (; k < arg_count - 1; ++k) {
            // i. Let nextArg be args[k].
            auto next_arg = args[k];

            // ii. Let nextArgString be ? ToString(nextArg).
            // iii. Set P to the string-concatenation of P, "," (a comma), and nextArgString.
            parameters.append(TRY(next_arg.to_byte_string(vm)));

            // iv. Set k to k + 1.
        }
        parameters_string = ByteString::join(',', parameters);

        // f. Let bodyArg be args[k].
        body_arg = args[k];
    }

    // 13. Let bodyString be the string-concatenation of 0x000A (LINE FEED), ? ToString(bodyArg), and 0x000A (LINE FEED).
    auto body_string = ByteString::formatted("\n{}\n", body_arg.has_value() ? TRY(body_arg->to_byte_string(vm)) : "");

    // 14. Let sourceString be the string-concatenation of prefix, " anonymous(", P, 0x000A (LINE FEED), ") {", bodyString, and "}".
    // 15. Let sourceText be StringToCodePoints(sourceString).
    auto source_text = ByteString::formatted("{} anonymous({}\n) {{{}}}", prefix, parameters_string, body_string);

    u8 parse_options = FunctionNodeParseOptions::CheckForFunctionAndName;
    if (kind == FunctionKind::Async || kind == FunctionKind::AsyncGenerator)
        parse_options |= FunctionNodeParseOptions::IsAsyncFunction;
    if (kind == FunctionKind::Generator || kind == FunctionKind::AsyncGenerator)
        parse_options |= FunctionNodeParseOptions::IsGeneratorFunction;

    // 16. Let parameters be ParseText(StringToCodePoints(P), parameterSym).
    i32 function_length = 0;
    auto parameters_parser = Parser { Lexer { parameters_string } };
    auto parameters = parameters_parser.parse_formal_parameters(function_length, parse_options);

    // 17. If parameters is a List of errors, throw a SyntaxError exception.
    if (parameters_parser.has_errors()) {
        auto error = parameters_parser.errors()[0];
        return vm.throw_completion<SyntaxError>(error.to_string());
    }

    // 18. Let body be ParseText(StringToCodePoints(bodyString), bodySym).
    FunctionParsingInsights parsing_insights;
    auto body_parser = Parser::parse_function_body_from_string(body_string, parse_options, parameters, kind, parsing_insights);

    // 19. If body is a List of errors, throw a SyntaxError exception.
    if (body_parser.has_errors()) {
        auto error = body_parser.errors()[0];
        return vm.throw_completion<SyntaxError>(error.to_string());
    }

    // 20. NOTE: The parameters and body are parsed separately to ensure that each is valid alone. For example, new Function("/*", "*/ ) {") is not legal.
    // 21. NOTE: If this step is reached, sourceText must have the syntax of exprSym (although the reverse implication does not hold). The purpose of the next two steps is to enforce any Early Error rules which apply to exprSym directly.

    // 22. Let expr be ParseText(sourceText, exprSym).
    auto source_parser = Parser { Lexer { source_text } };
    // This doesn't need any parse_options, it determines those & the function type based on the tokens that were found.
    auto expr = source_parser.parse_function_node<FunctionExpression>();

    // 23. If expr is a List of errors, throw a SyntaxError exception.
    if (source_parser.has_errors()) {
        auto error = source_parser.errors()[0];
        return vm.throw_completion<SyntaxError>(error.to_string());
    }

    // 24. Let proto be ? GetPrototypeFromConstructor(newTarget, fallbackProto).
    auto* prototype = TRY(get_prototype_from_constructor(vm, *new_target, fallback_prototype));

    // 25. Let realmF be the current Realm Record.
    auto& realm = *vm.current_realm();

    // 26. Let env be realmF.[[GlobalEnv]].
    auto& environment = realm.global_environment();

    // 27. Let privateEnv be null.
    PrivateEnvironment* private_environment = nullptr;

    // 28. Let F be OrdinaryFunctionCreate(proto, sourceText, parameters, body, non-lexical-this, env, privateEnv).
    parsing_insights.might_need_arguments_object = true;
    auto function = ECMAScriptFunctionObject::create(realm, "anonymous", *prototype, move(source_text), expr->body(), expr->parameters(), expr->function_length(), expr->local_variables_names(), &environment, private_environment, expr->kind(), expr->is_strict_mode(), parsing_insights);

    // FIXME: Remove the name argument from create() and do this instead.
    // 29. Perform SetFunctionName(F, "anonymous").

    // 30. If kind is generator, then
    if (kind == FunctionKind::Generator) {
        // a. Let prototype be OrdinaryObjectCreate(%GeneratorFunction.prototype.prototype%).
        prototype = Object::create_prototype(realm, realm.intrinsics().generator_function_prototype_prototype());

        // b. Perform ! DefinePropertyOrThrow(F, "prototype", PropertyDescriptor { [[Value]]: prototype, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }).
        function->define_direct_property(vm.names.prototype, prototype, Attribute::Writable);
    }
    // 31. Else if kind is asyncGenerator, then
    else if (kind == FunctionKind::AsyncGenerator) {
        // a. Let prototype be OrdinaryObjectCreate(%AsyncGeneratorFunction.prototype.prototype%).
        prototype = Object::create_prototype(realm, realm.intrinsics().async_generator_function_prototype_prototype());

        // b. Perform ! DefinePropertyOrThrow(F, "prototype", PropertyDescriptor { [[Value]]: prototype, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }).
        function->define_direct_property(vm.names.prototype, prototype, Attribute::Writable);
    }
    // 32. Else if kind is normal, perform MakeConstructor(F).
    else if (kind == FunctionKind::Normal) {
        function->make_constructor();
    }

    // 33. NOTE: Functions whose kind is async are not constructible and do not have a [[Construct]] internal method or a "prototype" property.

    // 34. Return F.
    return function.ptr();
}

// 20.2.1.1 Function ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
ThrowCompletionOr<Value> FunctionConstructor::call()
{
    return TRY(construct(*this));
}

// 20.2.1.1 Function ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
ThrowCompletionOr<NonnullGCPtr<Object>> FunctionConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 1. Let C be the active function object.
    auto* constructor = vm.active_function_object();

    // 2. Let args be the argumentsList that was passed to this function by [[Call]] or [[Construct]].
    MarkedVector<Value> args(heap());
    for (auto argument : vm.running_execution_context().arguments)
        args.append(argument);

    // 3. Return ? CreateDynamicFunction(C, NewTarget, normal, args).
    return *TRY(create_dynamic_function(vm, *constructor, &new_target, FunctionKind::Normal, args));
}

}
