/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Exported Functions, https://webassembly.github.io/spec/js-api/#exported-function, and the host functions that
//! JavaScript functions become when a module imports them, https://webassembly.github.io/spec/js-api/#create-a-host-function

use core::ffi::c_void;
use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{
    Attribute, DeferGc, ErrorKind, ErrorType, FunctionObject, Gc, MarkedVec, Must, NativeFunction, Object,
    PrimitiveString, PropertyAttributes, PropertyKey, Realm, ThrowCompletionOr, Trace, Value, Visitor, Vm, call,
    get_iterator_from_method_impl, iterator_to_list,
};
use libwasm_engine::runtime::heap::Visitor as StoreVisitor;
use libwasm_engine::runtime::{
    FunctionAddress, FunctionInstance, FunctionKind, HostCallback, HostFunction, Store, Trap, Value as WasmValue,
};
use libwasm_engine::types::{FuncType, ValType};

use crate::web_assembly::exception::Exception;
use crate::web_assembly::values::{
    EXTERNREF, matches_v128_or_exnref, results_to_js_value, to_js_value, to_webassembly_value,
};
use crate::web_assembly::{
    Agent, EntityRef, WasmValues, gc_visitor_visit_cell, host_object, set_host_object, throw_of_trap,
};

/// The internal slots of an Exported Function: its [[FunctionAddress]], and the type of its function.
struct ExportedFunction {
    function: EntityRef<FunctionInstance>,
    ty: FuncType,
    use_stack_slots: bool,
}

const STACK_SLOT_CAPACITY: usize = 8;

// SAFETY: The type holds no cells, so the function is the one cell the slots reach.
unsafe impl Trace for ExportedFunction {
    fn trace(&self, visitor: &mut Visitor) {
        self.function.trace(visitor);
    }
}

/// The value of the function's [[FunctionAddress]] internal slot, if it has one and is therefore an Exported Function.
pub(crate) fn exported_function_address(function: Gc<Object>) -> Option<FunctionAddress> {
    NativeFunction::captures_if::<ExportedFunction>(&function).map(|function| function.function.0)
}

/// The name of the WebAssembly function, https://webassembly.github.io/spec/js-api/#name-of-the-webassembly-function
fn name_of_webassembly_function(store: &Store, function: FunctionAddress) -> Utf16FlyString {
    // 1. Let store be the surrounding agent's associated store.
    // 2. Let funcinst be store.funcs[funcaddr].
    let index = match &store.function(function).kind {
        // 3. If funcinst is of the form {type functype, hostcode hostfunc},
        //     1. Assert: hostfunc is a JavaScript object and IsCallable(hostfunc) is true.
        //     2. Let index be the index of the host function funcaddr.
        // SAFETY: The data of the browser's host functions, the steps of a JavaScript function or of a builtin, is the
        //         index of the host function.
        FunctionKind::Host(steps) => unsafe { *steps.user_data().cast::<u32>() },
        // 4. Otherwise,
        //     1. Let moduleinst be funcinst.module.
        //     2. Assert: funcaddr is contained in moduleinst.funcaddrs.
        //     3. Let index be the index of moduleinst.funcaddrs where funcaddr is found.
        FunctionKind::Wasm { index, .. } => *index,
    };

    // 5. Return ! ToString(index).
    Utf16FlyString::from_utf8(&index.to_string())
}

/// https://webassembly.github.io/spec/js-api/#a-new-exported-function
pub(crate) fn create_exported_function(vm: &Vm, realm: Gc<Realm>, function: FunctionAddress) -> Gc<NativeFunction> {
    if let Some(object) = host_object::<NativeFunction>(function.cell()) {
        return object;
    }

    // 5. Let store be the surrounding agent's associated store.
    let store = Agent::the(vm).store();
    let ty = store.function(function).ty.clone();
    let arity = ty.params.len() as i32;
    let name = name_of_webassembly_function(store, function);
    let use_stack_slots = matches!(store.function(function).kind, FunctionKind::Wasm { .. })
        && ty.params.len() <= STACK_SLOT_CAPACITY
        && ty.results.len() <= STACK_SLOT_CAPACITY
        && ty
            .params
            .iter()
            .chain(&ty.results)
            .all(|ty| matches!(ty, ValType::I32 | ValType::I64 | ValType::F32 | ValType::F64));

    // NB: The function has its name on call stacks too, as a HostFunction of the C++ API would.
    let object = NativeFunction::create_with_name(
        vm,
        realm,
        &name,
        ExportedFunction {
            function: EntityRef(function),
            ty,
            use_stack_slots,
        },
        |vm: &Vm, function: &ExportedFunction| call_an_exported_function(vm, function),
    );
    let configurable = PropertyAttributes::new(Attribute::CONFIGURABLE);
    object.define_direct_property(vm, &vm.names.length, Value::from_i32(arity), configurable);
    let name_value = Value::from_string(PrimitiveString::create(vm, Utf16String::from(&name)));
    object.define_direct_property(vm, &vm.names.name, name_value, configurable);
    object.set_initial_name(name);

    set_host_object(function.cell(), object);

    object
}

/// https://webassembly.github.io/spec/js-api/#call-an-exported-function
fn call_an_exported_function(vm: &Vm, function: &ExportedFunction) -> ThrowCompletionOr<Value> {
    // 1. Let store be the surrounding agent's associated store.
    // 2. Let functype be func_type(store, funcaddr).
    // 3. Let func parameters → results be the expansion of functype.
    // NB: The slots keep functype, which never changes.
    let FuncType {
        params: parameters,
        results,
    } = &function.ty;

    // Numeric values need no GC roots. Keep small signatures on the stack and pass their slots directly to the
    // engine, avoiding the traced containers and temporary vectors of the general path. Convert arguments in
    // JavaScript order even though the engine's operand stack stores them in reverse order: conversion can run JS.
    if function.use_stack_slots {
        let mut argument_slots = [0; STACK_SLOT_CAPACITY];
        for (i, &ty) in parameters.iter().enumerate() {
            let value = to_webassembly_value(vm, vm.argument(i), ty)?;
            argument_slots[parameters.len() - i - 1] = value.to_slots().0[0];
        }
        let mut result_slots = [0; STACK_SLOT_CAPACITY];
        Agent::the(vm)
            .store()
            .invoke_slots(
                function.function.0,
                &argument_slots[..parameters.len()],
                &mut result_slots[..results.len()],
            )
            .map_err(|trap| throw_of_trap(vm, trap))?;
        let mut values = [WasmValue::I32(0); STACK_SLOT_CAPACITY];
        for (i, &ty) in results.iter().enumerate() {
            values[i] = WasmValue::from_slots(ty, &result_slots[results.len() - i - 1..]);
        }
        return results_to_js_value(vm, &values[..results.len()], results);
    }

    // 4. If any type in parameters or results matches v128 or exnref, throw a TypeError.
    // Note: the above error is thrown each time the [[Call]] method is invoked.
    if parameters.iter().chain(results).any(|&ty| matches_v128_or_exnref(ty)) {
        return vm.throw_completion_with_message(
            ErrorKind::TypeError,
            "Cannot call a WebAssembly function with v128 or exnref parameters or results from JavaScript".to_string(),
        );
    }

    // 5. Let args be « ».
    // NB: The converted arguments may refer to new cells, which the cell of the arguments keeps alive during the call.
    let args = WasmValues::allocate(vm);
    let arg_traces = WasmValues::traces_of(vm, parameters);

    // 6. Let i be 0.
    // 7. For each t of parameters,
    for (i, &t) in parameters.iter().enumerate() {
        // 1. If argValues's size > i, let arg be argValues[i].
        // 2. Otherwise, let arg be undefined.
        let arg = vm.argument(i);

        // 3. Append ? ToWebAssemblyValue(arg, t) to args.
        let arg = to_webassembly_value(vm, arg, t)?;
        args.push(arg, arg_traces[i]);

        // 4. Set i to i + 1.
    }

    // NB: The results may refer to new cells, which the cell of the results keeps alive from when the call returns them.
    let ret = WasmValues::allocate(vm);

    // 8. Let (store, ret) be the result of func_invoke(store, funcaddr, args).
    // 9. Set the surrounding agent's associated store to store.
    let result = Agent::the(vm).store().invoke(function.function.0, &args.values());
    // NB: Using the Gc after the call keeps the arguments where the conservative scan finds them until it has returned.
    core::hint::black_box(args);

    // 10. If ret is error, throw an exception. This exception should be a WebAssembly RuntimeError exception, unless
    //     otherwise indicated by the WebAssembly error mapping.
    // 11. If ret is of the form exception exnaddr, then
    // NB: throw_of_trap() throws what an error or an exception throws.
    match result {
        Ok(values) => ret.set(vm, values, results),
        Err(trap) => return Err(throw_of_trap(vm, trap)),
    }

    // 12. Let outArity be the size of ret.
    // 13. If outArity is 0, return undefined.
    // 14. Otherwise, if outArity is 1, return ToJSValue(ret[0]).
    // 15. Otherwise,
    //     1. Let values be « ».
    //     2. For each r of ret,
    //         1. Append ToJSValue(r) to values.
    //     3. Return CreateArrayFromList(values).
    let result = results_to_js_value(vm, &ret.values(), results);
    // NB: Using the Gc after the conversion keeps the results where the conservative scan finds them until it is done.
    core::hint::black_box(ret);
    result
}

/// The steps of a host function that a JavaScript function becomes, and the index of the host function.
struct HostFunctionSteps {
    function: Gc<FunctionObject>,
    ty: FuncType,
    index: u32,
    use_stack_arguments: bool,
}

impl HostFunction for HostFunctionSteps {
    // https://webassembly.github.io/spec/js-api/#create-a-host-function
    // NB: The store the engine passes is the agent's. The JavaScript function reaches it through the agent as well,
    //     and may call back into WebAssembly, so the steps reach it the same way and never through both at once.
    fn call(&self, _: &mut Store, arguments: &[WasmValue]) -> Result<Vec<WasmValue>, Trap> {
        let agent = Agent::current();
        let vm = agent.vm();
        // FIXME: The settings steps around the call.
        let result = run_a_host_function(vm, self.function, &self.ty, arguments, self.use_stack_arguments);

        let throw = match result {
            Ok(results) => return Ok(results),
            Err(throw) => throw,
        };
        let value = throw.value();

        if let Some(exception) = Exception::address_of_value(value) {
            return Err(Trap::UncaughtException(exception));
        }

        // 3. Otherwise,
        //     1. Let type be the JavaScript exception tag.
        //     2. Let payload be ! ToWebAssemblyValue(v, externref).
        //     3. Let (store, address) be exn_alloc(store, type, « payload »).
        // NB: The exception is v again once it leaves WebAssembly.
        let payload = to_webassembly_value(vm, value, EXTERNREF).must();
        Err(Trap::UncaughtException(
            agent.store().allocate_exception_with_values(agent.js_tag(), &[payload]),
        ))
    }

    fn visit_edges(&self, visitor: &StoreVisitor) {
        // SAFETY: The visitor is live while it visits, and the function is a cell of its heap.
        unsafe { gc_visitor_visit_cell(visitor.raw(), self.function.as_ptr().cast()) };
    }

    fn user_data(&self) -> *mut c_void {
        core::ptr::from_ref(&self.index).cast_mut().cast()
    }
}

/// https://webassembly.github.io/spec/js-api/#run-a-host-function
fn run_a_host_function(
    vm: &Vm,
    function: Gc<FunctionObject>,
    functype: &FuncType,
    arguments: &[WasmValue],
    use_stack_arguments: bool,
) -> ThrowCompletionOr<Vec<WasmValue>> {
    // 1. Let func parameters → results be the expansion of functype.
    let FuncType {
        params: parameters,
        results,
    } = functype;

    // 2. If any type in parameters or results matches v128 or exnref, throw a TypeError.
    if parameters.iter().chain(results).any(|&ty| matches_v128_or_exnref(ty)) {
        return vm.throw_completion_with_message(
            ErrorKind::TypeError,
            "Cannot call a JavaScript function with v128 or exnref parameters or results from WebAssembly".to_string(),
        );
    }

    let callee = Value::from_object(function.upcast::<Object>());

    // OPTIMIZATION: These conversions produce only Numbers, so the arguments need no GC roots.
    let ret = if use_stack_arguments {
        // 3. Let jsArguments be « ».
        let mut js_arguments = [Value::UNDEFINED; STACK_SLOT_CAPACITY];

        // 4. For each arg of arguments,
        for (i, (arg, &parameter)) in arguments.iter().zip(parameters).enumerate() {
            // 1. Append ToJSValue(arg) to jsArguments.
            js_arguments[i] = to_js_value(vm, *arg, parameter)?;
        }

        // 5. Let ret be ? Call(func, undefined, jsArguments).
        call(vm, callee, Value::UNDEFINED, &js_arguments[..arguments.len()])?
    } else {
        // 3. Let jsArguments be « ».
        let js_arguments = MarkedVec::with_capacity(vm, arguments.len());

        // 4. For each arg of arguments,
        for (arg, &parameter) in arguments.iter().zip(parameters) {
            // 1. Append ToJSValue(arg) to jsArguments.
            js_arguments.push(to_js_value(vm, *arg, parameter)?);
        }

        // 5. Let ret be ? Call(func, undefined, jsArguments).
        call(vm, callee, Value::UNDEFINED, &js_arguments.values())?
    };

    // 6. Let resultsSize be results's size.
    // 7. If resultsSize is 0, return « ».
    if results.is_empty() {
        return Ok(Vec::new());
    }

    // 8. Otherwise, if resultsSize is 1, return « ? ToWebAssemblyValue(ret, results[0]) ».
    if results.len() == 1 {
        let value = to_webassembly_value(vm, ret, results[0])?;
        // OPTIMIZATION: No further conversion or GC allocation can occur before the engine receives this value.
        return Ok(vec![value]);
    }

    // NB: Converting subsequent results can collect garbage, so keep earlier reference results in a traced cell.
    let converted = WasmValues::allocate(vm);
    let result_traces = WasmValues::traces_of(vm, results);

    // 9. Otherwise,
    {
        // 1. Let method be ? GetMethod(ret, %Symbol.iterator%).
        let method = ret.get_method(vm, &PropertyKey::from(vm.well_known_symbols().iterator))?;

        // 2. If method is undefined, throw a TypeError.
        let Some(method) = method else {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::NotIterable,
                &[&ret.to_utf16_string_without_side_effects()],
            );
        };

        // 3. Let values be ? IteratorToList(? GetIteratorFromMethod(ret, method)).
        let iterator = get_iterator_from_method_impl(vm, ret, method)?;
        let values = iterator_to_list(vm, &iterator)?;

        // 4. Let wasmValues be a new, empty list.
        // 5. If values's size is not resultsSize, throw a TypeError exception.
        if values.len() != results.len() {
            return vm.throw_completion_with_message(
                ErrorKind::TypeError,
                format!(
                    "Invalid number of return values for multi-value wasm return of {} objects",
                    results.len()
                ),
            );
        }

        // 6. For each value and resultType in values and results, paired linearly,
        for (index, &result_type) in results.iter().enumerate() {
            // 1. Append ? ToWebAssemblyValue(value, resultType) to wasmValues.
            let value = to_webassembly_value(
                vm,
                values.get(index).expect("the list has a value per result"),
                result_type,
            )?;
            converted.push(value, result_traces[index]);
        }

        // 7. Return wasmValues.
    }
    Ok(converted.take())
}

/// https://webassembly.github.io/spec/js-api/#create-a-host-function
pub(crate) fn create_host_function(vm: &Vm, function: Gc<FunctionObject>, ty: FuncType, index: u32) -> FunctionAddress {
    // 1. Assert: IsCallable(func).
    // 2. Let stored settings be the incumbent settings object.
    // 3. Let hostfunc be a host function which performs the following steps when called with arguments arguments:
    // NB: The function's entity visits what the steps hold, which keeps the JavaScript function alive with it. Until
    //     the entity holds them, nothing collects garbage.
    // 4. Let store be the surrounding agent's associated store.
    // 6. Set the surrounding agent's associated store to store.
    DeferGc::scope(vm, |_| {
        #[cfg_attr(
            ladybird_gc_lints,
            expect(
                gc_container_on_stack,
                reason = "the function's entity visits the steps through HostFunction::visit_edges()"
            )
        )]
        let steps: HostCallback = Rc::new(HostFunctionSteps {
            function,
            use_stack_arguments: ty.params.len() <= STACK_SLOT_CAPACITY
                && ty
                    .params
                    .iter()
                    .all(|ty| matches!(ty, ValType::I32 | ValType::F32 | ValType::F64)),
            ty: ty.clone(),
            index,
        });
        Agent::the(vm).store().allocate_host_function(ty, steps)
    })
}
