/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::bytecode::executable::Executable;
use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::run::should_dump_bytecode;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::async_function_driver_wrapper::AsyncFunctionDriverWrapper;
use crate::runtime::async_generator::AsyncGenerator;
use crate::runtime::class_field_definition::ClassElementName;
use crate::runtime::completion::{Throw, ThrowCompletionOr};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::generator_object::{GeneratingFunction, GeneratorObject};
use crate::runtime::native_function::{
    NATIVE_FUNCTION_METHODS, NATIVE_FUNCTION_VIRTUAL_METHODS, NativeFunction, NativeFunctionMethods,
};
use crate::runtime::object::{ObjectMethods, StackFrameInfo};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::shared_function_instance_data::{FunctionKind, SharedFunctionInstanceData, ThisMode};

/// A built-in function written in JavaScript, whose body the frontend compiles with the builtin abstract operations
/// enabled the first time it is called.
#[repr(C)]
#[derive(Trace)]
pub struct NativeJavaScriptBackedFunction {
    base: NativeFunction,
    shared_function_instance_data: Cell<Gc<SharedFunctionInstanceData>>,
}

static NATIVE_JAVASCRIPT_BACKED_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: NativeJavaScriptBackedFunction::call,
    function_environment_needed: |function| {
        as_native_javascript_backed_function(function).function_environment_needed()
    },
    function_environment_bindings_count: |function| {
        as_native_javascript_backed_function(function).function_environment_bindings_count()
    },
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

static NATIVE_JAVASCRIPT_BACKED_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    is_strict_mode: |object| as_native_javascript_backed_function(object).is_strict_mode(),
    get_stack_frame_info: NativeJavaScriptBackedFunction::get_stack_frame_info,
    native_function: Some(&NATIVE_JAVASCRIPT_BACKED_FUNCTION_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

define_cell!(
    NativeJavaScriptBackedFunction,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: NATIVE_JAVASCRIPT_BACKED_FUNCTION_METHODS
);

impl Deref for NativeJavaScriptBackedFunction {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

/// The function an internal method of a NativeJavaScriptBackedFunction was called on.
fn as_native_javascript_backed_function(object: &Object) -> &NativeJavaScriptBackedFunction {
    assert!(object.is::<NativeJavaScriptBackedFunction>());
    // SAFETY: The object is a NativeJavaScriptBackedFunction, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<NativeJavaScriptBackedFunction>() }
}

impl NativeJavaScriptBackedFunction {
    // 10.3.3 CreateBuiltinFunction ( behaviour, length, name, additionalInternalSlotsList [ , realm [ , prototype [ , prefix ] ] ] ), https://tc39.es/ecma262/#sec-createbuiltinfunction
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        shared_data: Gc<SharedFunctionInstanceData>,
        name: &PropertyKey,
        length: i32,
    ) -> Gc<NativeJavaScriptBackedFunction> {
        // 1. If realm is not present, set realm to the current Realm Record.
        // 2. If prototype is not present, set prototype to realm.[[Intrinsics]].[[%Function.prototype%]].
        let prototype = realm.function_prototype();

        // 3. Let internalSlotsList be a List containing the names of all the internal slots that 10.3 requires for the built-in function object that is about to be created.
        // 4. Append to internalSlotsList the elements of additionalInternalSlotsList.

        // 5. Let func be a new built-in function object that, when called, performs the action described by behaviour using the provided arguments as the values of the corresponding parameters specified by behaviour. The new function object has internal slots whose names are the elements of internalSlotsList, and an [[InitialName]] internal slot.
        // 6. Set func.[[Prototype]] to prototype.
        // 7. Set func.[[Extensible]] to true.
        // 8. Set func.[[Realm]] to realm.
        // 9. Set func.[[InitialName]] to null.
        let function = realm.create_object(
            vm,
            NativeJavaScriptBackedFunction {
                base: NativeFunction::new_with_name(vm, Self::CLASS, shared_data.name(), prototype),
                shared_function_instance_data: Cell::new(shared_data),
            },
        );

        function.unsafe_set_shape(realm.native_function_shape());

        // 10. Perform SetFunctionLength(func, length).
        function.put_direct(realm.native_function_length_offset(), Value::from_i32(length));

        // 11. If prefix is not present, then
        //     a. Perform SetFunctionName(func, name).
        // 12. Else,
        //     a. Perform SetFunctionName(func, name, prefix).
        let function_name = function.make_function_name(vm, &ClassElementName::PropertyKey(name.clone()), None);
        function.put_direct(realm.native_function_name_offset(), Value::from_string(function_name));

        // 13. Return func.
        function
    }

    pub fn as_native_javascript_backed_function_gc(&self) -> Gc<NativeJavaScriptBackedFunction> {
        // SAFETY: NativeJavaScriptBackedFunctions only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }

    fn get_stack_frame_info(object: &Object, vm: &Vm, stack_frame_info: &mut StackFrameInfo) {
        let function = as_native_javascript_backed_function(object);
        let bytecode_executable = function.bytecode_executable(vm);
        stack_frame_info.registers_and_locals_count = bytecode_executable.registers_and_locals_count();
        stack_frame_info.constant_count =
            u32::try_from(bytecode_executable.constants().len()).expect("the constant count fits in u32");
        // NB: This makes room for as many arguments as the function's length, where an ECMAScript function makes room
        //     for its formal parameters. The builtin files only declare functions whose two counts are the same.
        let function_length =
            u32::try_from(function.shared_data().function_length()).expect("a builtin's length is not negative");
        stack_frame_info.argument_count = stack_frame_info.argument_count.max(function_length);
    }

    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        let function = as_native_javascript_backed_function(function);

        let running_execution_context = vm
            .running_execution_context()
            .expect("a NativeJavaScriptBackedFunction runs in its own execution context");
        let result = vm
            .run_executable(running_execution_context, function.bytecode_executable(vm), 0)
            .map_err(Throw::new)?;

        let kind = function.kind();
        if kind == FunctionKind::Normal {
            return Ok(result);
        }

        let realm = vm
            .current_realm()
            .expect("a NativeJavaScriptBackedFunction runs in a realm");
        let generating_function =
            GeneratingFunction::NativeJavaScriptBacked(function.as_native_javascript_backed_function_gc());
        // SAFETY: The running execution context is live.
        let running_execution_context = unsafe { running_execution_context.as_ref() };
        if kind == FunctionKind::AsyncGenerator {
            return Ok(Value::from_object(AsyncGenerator::create(
                vm,
                realm,
                generating_function,
                running_execution_context.copy(),
            )));
        }

        let generator_object =
            GeneratorObject::create(vm, realm, generating_function, running_execution_context.copy());

        // NOTE: Async functions are entirely transformed to generator functions, and wrapped in a custom driver that returns a promise.
        if kind == FunctionKind::Async {
            return Ok(Value::from_object(AsyncFunctionDriverWrapper::create(
                vm,
                realm,
                generator_object,
            )));
        }

        assert!(kind == FunctionKind::Generator);
        Ok(Value::from_object(generator_object))
    }

    pub fn bytecode_executable(&self, vm: &Vm) -> Gc<Executable> {
        let shared_data = self.shared_data();
        if let Some(executable) = shared_data.executable() {
            return executable;
        }

        // NB: The abstract operations a builtin file calls are functions of the realm that is current while its
        //     executable is compiled, and ArraySpeciesCreate depends on its realm. The builtin may first be called from
        //     another realm, so it is compiled in its own.
        let running_execution_context = vm.running_execution_context();
        let caller_realm = running_execution_context.and_then(|context| {
            // SAFETY: The running execution context is live.
            let context = unsafe { context.as_ref() };
            context.realm.replace(Some(self.realm()))
        });
        let rust_executable = SharedFunctionInstanceData::compile_function(vm, shared_data, true)
            .expect("a builtin written in JavaScript compiles to an executable");
        if let Some(context) = running_execution_context {
            // SAFETY: As above.
            unsafe { context.as_ref() }.realm.set(caller_realm);
        }
        shared_data.set_executable(Some(rust_executable));
        rust_executable.set_name(shared_data.name());
        if should_dump_bytecode() {
            rust_executable.dump();
        }
        shared_data.clear_compile_inputs();
        rust_executable
    }

    /// The executable an inline call of this builtin runs, if the interpreter can call it inline: it is a normal
    /// function that needs no function environment.
    pub fn inline_call_executable(&self, vm: &Vm) -> Option<Gc<Executable>> {
        if self.kind() != FunctionKind::Normal || self.function_environment_needed() {
            return None;
        }
        Some(self.bytecode_executable(vm))
    }

    pub fn shared_data(&self) -> Gc<SharedFunctionInstanceData> {
        self.shared_function_instance_data.get()
    }

    pub fn kind(&self) -> FunctionKind {
        self.shared_data().kind()
    }

    pub fn this_mode(&self) -> ThisMode {
        self.shared_data().this_mode()
    }

    pub fn function_environment_needed(&self) -> bool {
        self.shared_data().function_environment_needed()
    }

    pub fn function_environment_bindings_count(&self) -> usize {
        self.shared_data().function_environment_bindings_count()
    }

    pub fn is_strict_mode(&self) -> bool {
        self.shared_data().strict()
    }
}
