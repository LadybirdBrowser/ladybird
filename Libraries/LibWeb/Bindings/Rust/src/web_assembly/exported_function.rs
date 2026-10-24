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
use libwasm_engine::types::FuncType;

use crate::web_assembly::exception::Exception;
use crate::web_assembly::values::{EXTERNREF, results_to_js_value, to_js_value, to_webassembly_value};
use crate::web_assembly::{
    Agent, EntityRef, WasmValues, gc_visitor_visit_cell, host_object, set_host_object, throw_of_trap,
};

/// The internal slots of an Exported Function: its [[FunctionAddress]], and the type of its function.
struct ExportedFunction {
    function: EntityRef<FunctionInstance>,
    ty: FuncType,
}

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

    // NB: The function has its name on call stacks too, as a HostFunction of the C++ API would.
    let object = NativeFunction::create_with_name(
        vm,
        realm,
        &name,
        ExportedFunction {
            function: EntityRef(function),
            ty,
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
    // NB: The conversions below throw that TypeError.

    // NB: The converted arguments may refer to new cells, which the cell of the arguments keeps alive during the call.
    let arguments = WasmValues::allocate(vm);

    for (index, &parameter) in function.ty.params.iter().enumerate() {
        let argument = to_webassembly_value(vm, vm.argument(index), parameter)?;
        arguments.0.borrow_mut().push(argument);
    }

    // NB: The results may refer to new cells, which the cell of the results keeps alive from when the call returns them.
    let results = WasmValues::allocate(vm);

    // 9. Set the surrounding agent's associated store to store.
    let result = Agent::the(vm)
        .store()
        .invoke(function.function.0, &arguments.0.borrow());
    // NB: Using the Gc after the call keeps the arguments where the conservative scan finds them until it has returned.
    core::hint::black_box(arguments);

    match result {
        Ok(values) => *results.0.borrow_mut() = values,
        Err(trap) => return Err(throw_of_trap(vm, trap)),
    }

    let result = results_to_js_value(vm, &results.0.borrow(), &function.ty.results);
    // NB: Using the Gc after the conversion keeps the results where the conservative scan finds them until it is done.
    core::hint::black_box(results);
    result
}

/// The steps of a host function that a JavaScript function becomes, and the index of the host function.
struct HostFunctionSteps {
    function: Gc<FunctionObject>,
    ty: FuncType,
    index: u32,
}

impl HostFunction for HostFunctionSteps {
    // https://webassembly.github.io/spec/js-api/#create-a-host-function
    // NB: The store the engine passes is the agent's. The JavaScript function reaches it through the agent as well,
    //     and may call back into WebAssembly, so the steps reach it the same way and never through both at once.
    fn call(&self, _: &mut Store, arguments: &[WasmValue]) -> Result<Vec<WasmValue>, Trap> {
        let agent = Agent::current();
        let vm = agent.vm();
        // FIXME: The settings steps around the call.
        let result = run_a_host_function(vm, self.function, &self.ty, arguments);

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
    ty: &FuncType,
    arguments: &[WasmValue],
) -> ThrowCompletionOr<Vec<WasmValue>> {
    // NB: The conversions below throw that TypeError.

    let js_arguments = MarkedVec::with_capacity(vm, arguments.len());

    for (argument, &parameter) in arguments.iter().zip(&ty.params) {
        js_arguments.push(to_js_value(vm, *argument, parameter)?);
    }

    let ret = call(
        vm,
        Value::from_object(function.upcast::<Object>()),
        Value::UNDEFINED,
        &js_arguments.values(),
    )?;

    if ty.results.is_empty() {
        return Ok(Vec::new());
    }

    // NB: The converted values may refer to new cells, which the cell of the values keeps alive until the engine has
    //     them.
    let converted = WasmValues::allocate(vm);

    if ty.results.len() == 1 {
        let value = to_webassembly_value(vm, ret, ty.results[0])?;
        converted.0.borrow_mut().push(value);
    } else {
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
        if values.len() != ty.results.len() {
            return vm.throw_completion_with_message(
                ErrorKind::TypeError,
                format!(
                    "Invalid number of return values for multi-value wasm return of {} objects",
                    ty.results.len()
                ),
            );
        }

        for (index, &result_type) in ty.results.iter().enumerate() {
            let value = to_webassembly_value(
                vm,
                values.get(index).expect("the list has a value per result"),
                result_type,
            )?;
            converted.0.borrow_mut().push(value);
        }

        // 7. Return wasmValues.
    }
    Ok(converted.0.take())
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
            ty: ty.clone(),
            index,
        });
        Agent::the(vm).store().allocate_host_function(ty, steps)
    })
}
