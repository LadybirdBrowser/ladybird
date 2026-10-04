/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::{CompilationType, Vm};
use crate::layout::cell::Gc;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::parser_error::ParserError;
use crate::runtime::abstract_operations::{IntrinsicDefaultPrototype, get_prototype_from_constructor};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::shared_function_instance_data::{FunctionKind, SharedFunctionInstanceData};
use crate::source_code::SourceCode;
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct FunctionConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    FunctionConstructor,
    initialize: FunctionConstructor::initialize,
    call: FunctionConstructor::call,
    construct: FunctionConstructor::construct
);

impl FunctionConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<FunctionConstructor> {
        realm.create_object(
            vm,
            FunctionConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Function.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 20.2.2.2 Function.prototype, https://tc39.es/ecma262/#sec-function.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.function_prototype()),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.2.1.1.1 CreateDynamicFunction ( constructor, newTarget, kind, parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-createdynamicfunction
    pub fn create_dynamic_function(
        vm: &Vm,
        constructor: Gc<FunctionObject>,
        new_target: Option<Gc<FunctionObject>>,
        kind: FunctionKind,
        parameter_args: &[Value],
        body_arg: Value,
    ) -> ThrowCompletionOr<Gc<EcmascriptFunctionObject>> {
        // 1. If newTarget is undefined, set newTarget to constructor.
        let new_target = new_target.unwrap_or(constructor);

        let prefix: &str;
        let fallback_prototype: IntrinsicDefaultPrototype;

        match kind {
            // 2. If kind is normal, then
            FunctionKind::Normal => {
                // a. Let prefix be "function".
                prefix = "function";

                // b. Let exprSym be the grammar symbol FunctionExpression.
                // c. Let bodySym be the grammar symbol FunctionBody[~Yield, ~Await].
                // d. Let parameterSym be the grammar symbol FormalParameters[~Yield, ~Await].

                // e. Let fallbackProto be "%Function.prototype%".
                fallback_prototype = Intrinsics::function_prototype;
            }

            // 3. Else if kind is generator, then
            FunctionKind::Generator => {
                // a. Let prefix be "function*".
                prefix = "function*";

                // b. Let exprSym be the grammar symbol GeneratorExpression.
                // c. Let bodySym be the grammar symbol GeneratorBody.
                // d. Let parameterSym be the grammar symbol FormalParameters[+Yield, ~Await].

                // e. Let fallbackProto be "%GeneratorFunction.prototype%".
                fallback_prototype = Intrinsics::generator_function_prototype;
            }

            // 4. Else if kind is async, then
            FunctionKind::Async => {
                // a. Let prefix be "async function".
                prefix = "async function";

                // b. Let exprSym be the grammar symbol AsyncFunctionExpression.
                // c. Let bodySym be the grammar symbol AsyncFunctionBody.
                // d. Let parameterSym be the grammar symbol FormalParameters[~Yield, +Await].

                // e. Let fallbackProto be "%AsyncFunction.prototype%".
                fallback_prototype = Intrinsics::async_function_prototype;
            }

            // 5. Else,
            FunctionKind::AsyncGenerator => {
                // a. Assert: kind is async-generator.

                // b. Let prefix be "async function*".
                prefix = "async function*";

                // c. Let exprSym be the grammar symbol AsyncGeneratorExpression.
                // d. Let bodySym be the grammar symbol AsyncGeneratorBody.
                // e. Let parameterSym be the grammar symbol FormalParameters[+Yield, +Await].

                // f. Let fallbackProto be "%AsyncGeneratorFunction.prototype%".
                fallback_prototype = Intrinsics::async_generator_function_prototype;
            }
        }

        // 6. Let argCount be the number of elements in parameterArgs.
        let arg_count = parameter_args.len();

        // 7. Let parameterStrings be a new empty List.
        let mut parameter_strings = Vec::with_capacity(arg_count);

        // 8. For each element arg of parameterArgs, do
        for parameter_value in parameter_args {
            // a. Append ? ToString(arg) to parameterStrings.
            parameter_strings.push(parameter_value.to_utf16_string(vm)?);
        }

        // 9. Let bodyString be ? ToString(bodyArg).
        let body_string = body_arg.to_utf16_string(vm)?;

        // 10. Let currentRealm be the current Realm Record.
        let realm = vm.current_realm().expect("a dynamic function is created in a realm");

        // 11. Let P be the empty String.
        let mut parameters_string: Vec<u16> = Vec::new();

        // 12. If argCount > 0, then
        //     a. Set P to parameterStrings[0].
        //     b. Let k be 1.
        //     c. Repeat, while k < argCount,
        //         i. Let nextArgString be parameterStrings[k].
        //         ii. Set P to the string-concatenation of P, "," (a comma), and nextArgString.
        //         iii. Set k to k + 1.
        for (index, parameter_string) in parameter_strings.iter().enumerate() {
            if index > 0 {
                parameters_string.push(u16::from(b','));
            }
            Utf16View::of_string(parameter_string).append_to(&mut parameters_string);
        }

        // 13. Let bodyParseString be the string-concatenation of 0x000A (LINE FEED), bodyString, and 0x000A (LINE FEED).
        let mut body_parse_string: Vec<u16> = vec![u16::from(b'\n')];
        Utf16View::of_string(&body_string).append_to(&mut body_parse_string);
        body_parse_string.push(u16::from(b'\n'));

        // 14. Let sourceString be the string-concatenation of prefix, " anonymous(", P, 0x000A (LINE FEED), ") {", bodyParseString, and "}".
        // 15. Let sourceText be StringToCodePoints(sourceString).
        let mut source_text: Vec<u16> = prefix.encode_utf16().collect();
        source_text.extend(" anonymous(".encode_utf16());
        source_text.extend_from_slice(&parameters_string);
        source_text.extend("\n) {".encode_utf16());
        source_text.extend_from_slice(&body_parse_string);
        source_text.push(u16::from(b'}'));
        let source_text = Utf16String::from_utf16(&source_text);

        // 16. Perform ? HostEnsureCanCompileStrings(currentRealm, parameterStrings, bodyString, sourceString, FUNCTION, parameterArgs, bodyArg).
        vm.host_ensure_can_compile_strings()(
            vm,
            realm,
            &parameter_strings,
            Utf16View::of_string(&body_string),
            Utf16View::of_string(&source_text),
            CompilationType::Function,
            parameter_args,
            body_arg,
        )?;

        let function_data =
            match compile_dynamic_function(vm, &source_text, &parameters_string, &body_parse_string, kind) {
                Ok(function_data) => function_data,
                Err(parser_error) => {
                    return vm.throw_completion_with_message(ErrorKind::SyntaxError, parser_error.to_string());
                }
            };

        // 25. Let proto be ? GetPrototypeFromConstructor(newTarget, fallbackProto).
        let mut prototype = get_prototype_from_constructor(vm, new_target, fallback_prototype)?;

        // 26. Let env be currentRealm.[[GlobalEnv]].
        let environment = realm.global_environment();

        // 27. Let privateEnv be null.
        let private_environment = None;

        let function = EcmascriptFunctionObject::create_from_function_data_with_prototype(
            vm,
            realm,
            function_data,
            Some(environment.upcast()),
            private_environment,
            prototype,
        );

        // FIXME: Remove the name argument from create() and do this instead.
        // 29. Perform SetFunctionName(F, "anonymous").

        let names = &vm.names;

        // 30. If kind is generator, then
        if kind == FunctionKind::Generator {
            // a. Let prototype be OrdinaryObjectCreate(%GeneratorFunction.prototype.prototype%).
            prototype = Object::create_prototype(vm, realm, Some(realm.generator_function_prototype_prototype()));

            // b. Perform ! DefinePropertyOrThrow(F, "prototype", PropertyDescriptor { [[Value]]: prototype, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }).
            function.define_direct_property(
                vm,
                &names.prototype,
                Value::from_object(prototype),
                PropertyAttributes::new(Attribute::WRITABLE),
            );
        }
        // 31. Else if kind is asyncGenerator, then
        else if kind == FunctionKind::AsyncGenerator {
            // a. Let prototype be OrdinaryObjectCreate(%AsyncGeneratorFunction.prototype.prototype%).
            prototype = Object::create_prototype(vm, realm, Some(realm.async_generator_function_prototype_prototype()));

            // b. Perform ! DefinePropertyOrThrow(F, "prototype", PropertyDescriptor { [[Value]]: prototype, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }).
            function.define_direct_property(
                vm,
                &names.prototype,
                Value::from_object(prototype),
                PropertyAttributes::new(Attribute::WRITABLE),
            );
        }
        // 32. Else if kind is normal, perform MakeConstructor(F).
        else if kind == FunctionKind::Normal {
            // FIXME: Implement MakeConstructor
            prototype = Object::create_prototype(vm, realm, Some(realm.object_prototype()));
            prototype.define_direct_property(
                vm,
                &names.constructor,
                Value::from_object(function),
                PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            );
            function.define_direct_property(
                vm,
                &names.prototype,
                Value::from_object(prototype),
                PropertyAttributes::new(Attribute::WRITABLE),
            );
        }

        // 33. NOTE: Functions whose kind is async are not constructible and do not have a [[Construct]] internal method or a "prototype" property.

        // 34. Return F.
        Ok(function)
    }

    // 20.2.1.1 Function ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(Self::construct(
            function,
            vm,
            function.as_function_object_gc(),
        )?))
    }

    // 20.2.1.1 Function ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let marked_arguments = MarkedVec::with_capacity(vm, vm.argument_count());
        for index in 0..vm.argument_count() {
            marked_arguments.push(vm.argument(index));
        }
        let arguments = marked_arguments.to_vec();

        let parameter_args = &arguments[..arguments.len().saturating_sub(1)];

        // 1. Let C be the active function object.
        let constructor = vm
            .active_function_object()
            .expect("a native function is the active function object");

        // 2. If bodyArg is not present, set bodyArg to the empty String.
        let body_arg = arguments
            .last()
            .copied()
            .unwrap_or_else(|| Value::from_string(vm.empty_string()));

        // 3. Return ? CreateDynamicFunction(C, NewTarget, normal, parameterArgs, bodyArg).
        Ok(Self::create_dynamic_function(
            vm,
            constructor,
            Some(new_target),
            FunctionKind::Normal,
            parameter_args,
            body_arg,
        )?
        .upcast())
    }
}

/// Steps 17 to 24 of CreateDynamicFunction, which RustIntegration::compile_dynamic_function performs: the shared data of
/// the function `source_text` defines, after checking its parameters and its body on their own, or the first error any
/// of these steps reports, which CreateDynamicFunction throws as a SyntaxError. Hosts compile event handlers and
/// WebDriver scripts through this as well.
pub fn compile_dynamic_function(
    vm: &Vm,
    source_text: &Utf16String,
    parameters_string: &[u16],
    body_parse_string: &[u16],
    kind: FunctionKind,
) -> Result<Gc<SharedFunctionInstanceData>, ParserError> {
    let source_code = SourceCode::create(Utf16String::default(), source_text.clone());
    let mut full_source = Vec::with_capacity(source_code.length_in_code_units());
    Utf16View::of_string(source_code.code()).append_to(&mut full_source);

    let description =
        libjs_rust::compile::parse_dynamic_function(&full_source, parameters_string, body_parse_string, kind)
            .and_then(libjs_rust::compile::ParsedDynamicFunction::into_description);
    let description = match description {
        Ok(description) => description,
        Err(errors) => {
            let error = errors.first().expect("a failed compilation reports an error");
            return Err(ParserError {
                message: error.message.clone(),
                line: error.line,
                column: error.column,
            });
        }
    };

    let function_data = SharedFunctionInstanceData::create(vm, description, Some(&source_code));
    function_data.set_source_text_owner(source_text.clone());
    Ok(function_data)
}
