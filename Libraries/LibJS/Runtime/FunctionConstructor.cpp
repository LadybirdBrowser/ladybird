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

GC_DEFINE_ALLOCATOR(FunctionConstructor);

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

// NON-STANDARD: Exists to simplify calling CreateDynamicFunction using strong types, instead of a Value.
// Analogous to parts of the following two AO's - and basically just extracts the body and parameters as strings.
//
// 20.2.1.1 Function ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
// 20.2.1.1.1 CreateDynamicFunction ( constructor, newTarget, kind, parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-createdynamicfunction
ThrowCompletionOr<ParameterArgumentsAndBody> extract_parameter_arguments_and_body(VM& vm, Span<Value> arguments)
{
    if (arguments.is_empty())
        return ParameterArgumentsAndBody {};

    auto parameter_values = arguments.slice(0, arguments.size() - 1);

    Vector<String> parameters;
    parameters.ensure_capacity(parameter_values.size());
    for (auto const& parameter_value : parameter_values)
        parameters.unchecked_append(TRY(parameter_value.to_string(vm)));

    auto body = TRY(arguments.last().to_string(vm));

    return ParameterArgumentsAndBody {
        .parameters = move(parameters),
        .body = move(body),
    };
}

// 20.2.1.1.1 CreateDynamicFunction ( constructor, newTarget, kind, parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-createdynamicfunction
ThrowCompletionOr<GC::Ref<ECMAScriptFunctionObject>> FunctionConstructor::create_dynamic_function(VM& vm, FunctionObject& constructor, FunctionObject* new_target, FunctionKind kind, ReadonlySpan<String> parameter_strings, String const& body_string)
{
    // 1. If newTarget is undefined, set newTarget to constructor.
    if (new_target == nullptr)
        new_target = &constructor;

    StringView prefix;
    GC::Ref<Object> (Intrinsics::*fallback_prototype)() = nullptr;

    switch (kind) {
    // 2. If kind is normal, then
    case FunctionKind::Normal:
        // a. Let prefix be "function".
        prefix = "function"sv;

        // b. Let exprSym be the grammar symbol FunctionExpression.
        // c. Let bodySym be the grammar symbol FunctionBody[~Yield, ~Await].
        // d. Let parameterSym be the grammar symbol FormalParameters[~Yield, ~Await].

        // e. Let fallbackProto be "%Function.prototype%".
        fallback_prototype = &Intrinsics::function_prototype;
        break;

    // 3. Else if kind is generator, then
    case FunctionKind::Generator:
        // a. Let prefix be "function*".
        prefix = "function*"sv;

        // b. Let exprSym be the grammar symbol GeneratorExpression.
        // c. Let bodySym be the grammar symbol GeneratorBody.
        // d. Let parameterSym be the grammar symbol FormalParameters[+Yield, ~Await].

        // e. Let fallbackProto be "%GeneratorFunction.prototype%".
        fallback_prototype = &Intrinsics::generator_function_prototype;
        break;

    // 4. Else if kind is async, then
    case FunctionKind::Async:
        // a. Let prefix be "async function".
        prefix = "async function"sv;

        // b. Let exprSym be the grammar symbol AsyncFunctionExpression.
        // c. Let bodySym be the grammar symbol AsyncFunctionBody.
        // d. Let parameterSym be the grammar symbol FormalParameters[~Yield, +Await].

        // e. Let fallbackProto be "%AsyncFunction.prototype%".
        fallback_prototype = &Intrinsics::async_function_prototype;
        break;

    // 5. Else,
    case FunctionKind::AsyncGenerator:
        // a. Assert: kind is async-generator.

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

    // 6. Let argCount be the number of elements in parameterArgs.
    auto arg_count = parameter_strings.size();

    // NOTE: Done by caller
    // 7. Let parameterStrings be a new empty List.
    // 8. For each element arg of parameterArgs, do
    //     a. Append ? ToString(arg) to parameterStrings.
    // 9. Let bodyString be ? ToString(bodyArg).

    // 10. Let currentRealm be the current Realm Record.
    auto& realm = *vm.current_realm();

    // 11. Perform ? HostEnsureCanCompileStrings(currentRealm, parameterStrings, bodyString, false).
    TRY(vm.host_ensure_can_compile_strings(realm, parameter_strings, body_string, EvalMode::Indirect));

    // 12. Let P be the empty String.
    String parameters_string;

    // 13. If argCount > 0, then
    if (arg_count > 0) {
        // a. Set P to parameterStrings[0].
        // b. Let k be 1.
        // c. Repeat, while k < argCount,
        //     i. Let nextArgString be parameterStrings[k].
        //     ii. Set P to the string-concatenation of P, "," (a comma), and nextArgString.
        //     iii. Set k to k + 1.
        parameters_string = MUST(String::join(',', parameter_strings));
    }

    // 14. Let bodyParseString be the string-concatenation of 0x000A (LINE FEED), bodyString, and 0x000A (LINE FEED).
    auto body_parse_string = ByteString::formatted("\n{}\n", body_string);

    // 15. Let sourceString be the string-concatenation of prefix, " anonymous(", P, 0x000A (LINE FEED), ") {", bodyParseString, and "}".
    // 16. Let sourceText be StringToCodePoints(sourceString).
    auto source_text = ByteString::formatted("{} anonymous({}\n) {{{}}}", prefix, parameters_string, body_parse_string);

    u8 parse_options = FunctionNodeParseOptions::CheckForFunctionAndName;
    if (kind == FunctionKind::Async || kind == FunctionKind::AsyncGenerator)
        parse_options |= FunctionNodeParseOptions::IsAsyncFunction;
    if (kind == FunctionKind::Generator || kind == FunctionKind::AsyncGenerator)
        parse_options |= FunctionNodeParseOptions::IsGeneratorFunction;

    // 17. Let parameters be ParseText(P, parameterSym).
    i32 function_length = 0;
    auto parameters_parser = Parser { Lexer { parameters_string } };
    auto parameters = parameters_parser.parse_formal_parameters(function_length, parse_options);

    // 18. If parameters is a List of errors, throw a SyntaxError exception.
    if (parameters_parser.has_errors()) {
        auto error = parameters_parser.errors()[0];
        return vm.throw_completion<SyntaxError>(error.to_string());
    }

    // 19. Let body be ParseText(bodyParseString, bodySym).
    FunctionParsingInsights parsing_insights;
    auto body_parser = Parser::parse_function_body_from_string(body_parse_string, parse_options, parameters, kind, parsing_insights);

    // 20. If body is a List of errors, throw a SyntaxError exception.
    if (body_parser.has_errors()) {
        auto error = body_parser.errors()[0];
        return vm.throw_completion<SyntaxError>(error.to_string());
    }

    // 21. NOTE: The parameters and body are parsed separately to ensure that each is valid alone. For example, new Function("/*", "*/ ) {") does not evaluate to a function.
    // 22. NOTE: If this step is reached, sourceText must have the syntax of exprSym (although the reverse implication does not hold). The purpose of the next two steps is to enforce any Early Error rules which apply to exprSym directly.

    // 23. Let expr be ParseText(sourceText, exprSym).
    auto source_parser = Parser { Lexer { source_text } };
    // This doesn't need any parse_options, it determines those & the function type based on the tokens that were found.
    auto expr = source_parser.parse_function_node<FunctionExpression>();

    // 24. If expr is a List of errors, throw a SyntaxError exception.
    if (source_parser.has_errors()) {
        auto error = source_parser.errors()[0];
        return vm.throw_completion<SyntaxError>(error.to_string());
    }

    // 25. Let proto be ? GetPrototypeFromConstructor(newTarget, fallbackProto).
    auto* prototype = TRY(get_prototype_from_constructor(vm, *new_target, fallback_prototype));

    // 26. Let env be currentRealm.[[GlobalEnv]].
    auto& environment = realm.global_environment();

    // 27. Let privateEnv be null.
    PrivateEnvironment* private_environment = nullptr;

    // 28. Let F be OrdinaryFunctionCreate(proto, sourceText, parameters, body, non-lexical-this, env, privateEnv).
    parsing_insights.might_need_arguments_object = true;
    auto function = ECMAScriptFunctionObject::create(realm, "anonymous"_fly_string, *prototype, move(source_text), expr->body(), expr->parameters(), expr->function_length(), expr->local_variables_names(), &environment, private_environment, expr->kind(), expr->is_strict_mode(), parsing_insights);

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
        // FIXME: Implement MakeConstructor
        prototype = Object::create_prototype(realm, realm.intrinsics().object_prototype());
        prototype->define_direct_property(vm.names.constructor, function, Attribute::Writable | Attribute::Configurable);
        function->define_direct_property(vm.names.prototype, prototype, Attribute::Writable);
    }

    // 33. NOTE: Functions whose kind is async are not constructible and do not have a [[Construct]] internal method or a "prototype" property.

    // 34. Return F.
    return function;
}

// 20.2.1.1 Function ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
ThrowCompletionOr<Value> FunctionConstructor::call()
{
    return TRY(construct(*this));
}

// 20.2.1.1 Function ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
ThrowCompletionOr<GC::Ref<Object>> FunctionConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 1. Let C be the active function object.
    auto* constructor = vm.active_function_object();

    // 2. If bodyArg is not present, set bodyArg to the empty String.
    // NOTE: This does that, as well as the string extraction done inside of CreateDynamicFunction
    auto extracted = TRY(extract_parameter_arguments_and_body(vm, vm.running_execution_context().arguments));

    // 3. Return ? CreateDynamicFunction(C, NewTarget, normal, parameterArgs, bodyArg).
    return TRY(create_dynamic_function(vm, *constructor, &new_target, FunctionKind::Normal, extracted.parameters, extracted.body));
}

}
