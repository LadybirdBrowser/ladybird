/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell};
use core::ops::Deref;
use core::ptr::NonNull;

use ak::{Utf16FlyString, Utf16String};
use libjs_abi::Builtin;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
pub use crate::layout::function_object::{
    DirectGetterFunction, NativeFunction, NativeFunctionTableEntry, NativeFunctionType, RawNativeFunction,
};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::layout_forward::FlyStringSlot;
pub use crate::layout_forward::{RawNativeFunctionPointer, RawNativeFunctionResult};
use crate::runtime::abstract_operations::new_function_environment_for_native_javascript_backed_function;
use crate::runtime::class_field_definition::ClassElementName;
use crate::runtime::completion::{Throw, ThrowCompletionOr};
use crate::runtime::function_object::{FUNCTION_OBJECT_METHODS, FunctionObject};
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ObjectMethods, allocate_object};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// The virtual methods of the C++ NativeFunction, which the classes extending it override.
pub struct NativeFunctionMethods {
    /// Used for [[Call]] / [[Construct]]'s "...result of evaluating F in a manner that conforms to the specification of F".
    pub call: fn(&NativeFunction, &Vm) -> ThrowCompletionOr<Value>,
    pub construct: fn(&NativeFunction, &Vm, Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>>,
    pub function_environment_needed: fn(&NativeFunction) -> bool,
    pub function_environment_bindings_count: fn(&NativeFunction) -> usize,
}

pub static NATIVE_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: |_, _| unreachable!("NativeFunction::call has to be overridden"),
    // Needs to be overridden if [[Construct]] is needed.
    construct: |_, _, _| unreachable!("NativeFunction::construct has to be overridden"),
    function_environment_needed: |_| false,
    function_environment_bindings_count: |_| 0,
};

pub static NATIVE_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    internal_call: Some(NativeFunction::internal_call),
    internal_construct: Some(NativeFunction::internal_construct),
    has_constructor: |_| false,
    is_strict_mode: |_| true,
    function_realm: |object| Some(as_native_function(object).realm()),
    name_for_call_stack: |object| as_native_function(object).name_for_call_stack(),
    native_function: Some(&NATIVE_FUNCTION_VIRTUAL_METHODS),
    ..FUNCTION_OBJECT_METHODS
};

define_cell!(NativeFunction, Object, extends: [FunctionObject, Object], methods: NATIVE_FUNCTION_METHODS);

// SAFETY: The realm is the only cell a NativeFunction adds to its FunctionObject; the names are strings.
unsafe impl Trace for NativeFunction {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
        self.realm.trace(visitor);
    }
}

impl Deref for NativeFunction {
    type Target = FunctionObject;

    fn deref(&self) -> &FunctionObject {
        &self.base
    }
}

/// as<NativeJavaScriptBackedFunction>(*this): only a NativeJavaScriptBackedFunction needs a function environment.
fn as_native_javascript_backed_function(function: &NativeFunction) -> Gc<NativeJavaScriptBackedFunction> {
    function
        .as_native_function_gc()
        .downcast::<NativeJavaScriptBackedFunction>()
        .expect("only a NativeJavaScriptBackedFunction needs a function environment")
}

/// The native function an internal method of a native function was called on.
fn as_native_function(object: &Object) -> &NativeFunction {
    assert!(object.is::<NativeFunction>());
    // SAFETY: The object is a NativeFunction, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<NativeFunction>() }
}

impl NativeFunction {
    /// NativeFunction(GC::Ptr<Object> prototype, Realm& realm, Optional<Bytecode::Builtin> builtin)
    pub fn new_with_realm(
        vm: &Vm,
        class: &'static Class,
        prototype: Option<Gc<Object>>,
        realm: Gc<Realm>,
        builtin: Option<Builtin>,
    ) -> NativeFunction {
        let base = FunctionObject::new_with_realm_and_prototype(
            vm,
            class,
            realm,
            prototype,
            MayInterfereWithIndexedPropertyAccess::No,
        );
        base.set_builtin(builtin);
        Self::from_function_object(base, None, realm)
    }

    // FIXME: m_realm is supposed to be the realm argument of CreateBuiltinFunction, or the current
    //        Realm Record. The former is not something that's commonly used or we support, the
    //        latter is impossible as no ExecutionContext exists when most NativeFunctions are created...

    /// NativeFunction(Object& prototype)
    pub fn new_with_prototype(vm: &Vm, class: &'static Class, prototype: Gc<Object>) -> NativeFunction {
        let base = FunctionObject::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::No);
        Self::from_function_object(base, None, prototype.shape().realm())
    }

    /// NativeFunction(Utf16FlyString name, Object& prototype)
    pub fn new_with_name(
        vm: &Vm,
        class: &'static Class,
        name: Utf16FlyString,
        prototype: Gc<Object>,
    ) -> NativeFunction {
        let base = FunctionObject::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::No);
        Self::from_function_object(base, Some(name), prototype.shape().realm())
    }

    fn from_function_object(base: FunctionObject, name: Option<Utf16FlyString>, realm: Gc<Realm>) -> NativeFunction {
        NativeFunction {
            base,
            name: FlyStringSlot::new(name),
            initial_name: FlyStringSlot::new(None),
            realm: Cell::new(realm),
        }
    }

    // 10.3.3 CreateBuiltinFunction ( behaviour, length, name, additionalInternalSlotsList [ , realm [ , prototype [ , prefix ] ] ] ), https://tc39.es/ecma262/#sec-createbuiltinfunction
    // NOTE: This doesn't consider additionalInternalSlotsList, which is rarely used, and can either be implemented using only the `function` lambda, or needs a NativeFunction subclass.
    /// A native function with a behaviour that captures state. The behaviour itself must not capture anything: the
    /// state it needs is `captures`, which the function keeps alive.
    #[allow(clippy::too_many_arguments)]
    pub fn create<C, F>(
        vm: &Vm,
        captures: C,
        behaviour: F,
        length: i32,
        name: &PropertyKey,
        realm: Option<Gc<Realm>>,
        prefix: Option<&str>,
        builtin: Option<Builtin>,
    ) -> Gc<NativeFunction>
    where
        C: Trace + 'static,
        F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + 'static,
    {
        // 1. If realm is not present, set realm to the current Realm Record.
        let realm = realm.unwrap_or_else(|| vm.current_realm().expect("there is a current realm"));

        // 2. If prototype is not present, set prototype to realm.[[Intrinsics]].[[%Function.prototype%]].
        let prototype = realm.function_prototype();

        // 3. Let internalSlotsList be a List containing the names of all the internal slots that 10.3 requires for the built-in function object that is about to be created.
        // 4. Append to internalSlotsList the elements of additionalInternalSlotsList.

        // 5. Let func be a new built-in function object that, when called, performs the action described by behaviour using the provided arguments as the values of the corresponding parameters specified by behaviour. The new function object has internal slots whose names are the elements of internalSlotsList, and an [[InitialName]] internal slot.
        // 6. Set func.[[Prototype]] to prototype.
        // 7. Set func.[[Extensible]] to true.
        // 8. Set func.[[Realm]] to realm.
        // 9. Set func.[[InitialName]] to null.
        let function = CapturingNativeFunction::create(
            vm,
            NativeFunction::new_with_realm(vm, CapturingNativeFunction::CLASS, Some(prototype), realm, builtin),
            captures,
            behaviour,
        );

        function.unsafe_set_shape(realm.native_function_shape());

        // 10. Perform SetFunctionLength(func, length).
        function.put_direct(realm.native_function_length_offset(), Value::from_i32(length));

        // 11. If prefix is not present, then
        //     a. Perform SetFunctionName(func, name).
        // 12. Else,
        //     a. Perform SetFunctionName(func, name, prefix).
        let function_name = function.make_function_name(vm, &ClassElementName::PropertyKey(name.clone()), prefix);
        function.put_direct(realm.native_function_name_offset(), Value::from_string(function_name));

        // 13. Return func.
        function
    }

    /// NativeFunction::create(realm, behaviour, length) of C++: CreateBuiltinFunction(behaviour, length, "", « »).
    pub fn create_anonymous<C, F>(vm: &Vm, captures: C, behaviour: F, length: i32) -> Gc<NativeFunction>
    where
        C: Trace + 'static,
        F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + 'static,
    {
        Self::create(
            vm,
            captures,
            behaviour,
            length,
            &PropertyKey::from(Utf16FlyString::default()),
            None,
            None,
            None,
        )
    }

    /// A native function with a behaviour that captures state, and a name only call stacks show.
    pub fn create_with_name<C, F>(
        vm: &Vm,
        realm: Gc<Realm>,
        name: &Utf16FlyString,
        captures: C,
        behaviour: F,
    ) -> Gc<NativeFunction>
    where
        C: Trace + 'static,
        F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + 'static,
    {
        CapturingNativeFunction::create(
            vm,
            NativeFunction::new_with_name(
                vm,
                CapturingNativeFunction::CLASS,
                name.clone(),
                realm.function_prototype(),
            ),
            captures,
            behaviour,
        )
    }

    pub fn as_native_function_gc(&self) -> Gc<NativeFunction> {
        // SAFETY: Native functions only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }

    fn methods(&self) -> &'static NativeFunctionMethods {
        self.native_function_methods()
            .expect("a native function class has the native function methods")
    }

    // 10.3.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-built-in-function-objects-call-thisargument-argumentslist
    fn internal_call(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        this_argument: Value,
    ) -> ThrowCompletionOr<Value> {
        let function = as_native_function(object);

        // 1. Let callerContext be the running execution context.
        let caller_context = vm
            .running_execution_context()
            .expect("a native function is called from a running execution context");
        // SAFETY: The running execution context is live.
        let caller_context = unsafe { caller_context.as_ref() };

        // 2. If callerContext is not already suspended, suspend callerContext.
        // 3. Let calleeContext be a new execution context.

        // 4. Set the Function of calleeContext to F.
        callee_context.function.set(Some(function.as_function_object_gc()));

        // 5. Let calleeRealm be F.[[Realm]].
        // 6. Set the Realm of calleeContext to calleeRealm.
        callee_context.realm.set(Some(function.realm()));

        // 7. Set the ScriptOrModule of calleeContext to null.
        // Note: This is already the default value.

        // 8. Perform any necessary implementation-defined initialization of calleeContext.
        callee_context.this_value.set(this_argument);

        if function.function_environment_needed() {
            // 7. Let localEnv be NewFunctionEnvironment(F, newTarget).
            let native_javascript_backed_function = as_native_javascript_backed_function(function);
            let local_environment = new_function_environment_for_native_javascript_backed_function(
                vm,
                native_javascript_backed_function,
                None,
            );
            let shared_data = native_javascript_backed_function.shared_data();
            let function_environment_bindings_count = shared_data.function_environment_bindings_count();
            local_environment.set_environment_shape_cache(
                shared_data.function_environment_shape_cache(),
                function_environment_bindings_count,
            );
            local_environment.ensure_capacity(function_environment_bindings_count);

            // 8. Set the LexicalEnvironment of calleeContext to localEnv.
            callee_context.lexical_environment.set(Some(local_environment.upcast()));

            // 9. Set the VariableEnvironment of calleeContext to localEnv.
            callee_context
                .variable_environment
                .set(Some(local_environment.upcast()));
        } else {
            callee_context
                .lexical_environment
                .set(caller_context.lexical_environment.get());
            callee_context
                .variable_environment
                .set(caller_context.variable_environment.get());
        }

        // Note: Keeping the private environment is probably only needed because of async methods in classes
        //       calling async_block_start which goes through a NativeFunction here.
        callee_context
            .private_environment
            .set(caller_context.private_environment.get());

        // </8.> --------------------------------------------------------------------------

        // 9. Push calleeContext onto the execution context stack; calleeContext is now the running execution context.
        vm.push_execution_context_checking_stack_space(NonNull::from(callee_context))?;

        // 10. Let result be the Completion Record that is the result of evaluating F in a manner that conforms to the specification of F. thisArgument is the this value, argumentsList provides the named parameters, and the NewTarget value is undefined.
        let result = function.call(vm);

        // 11. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
        vm.pop_execution_context();

        // 12. Return ? result.
        result
    }

    // 10.3.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-built-in-function-objects-construct-argumentslist-newtarget
    fn internal_construct(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        new_target: Gc<FunctionObject>,
    ) -> ThrowCompletionOr<Gc<Object>> {
        let function = as_native_function(object);

        // 1. Let callerContext be the running execution context.
        let caller_context = vm
            .running_execution_context()
            .expect("a native function is constructed from a running execution context");
        // SAFETY: The running execution context is live.
        let caller_context = unsafe { caller_context.as_ref() };

        // 2. If callerContext is not already suspended, suspend callerContext.
        // 3. Let calleeContext be a new execution context.

        // 4. Set the Function of calleeContext to F.
        callee_context.function.set(Some(function.as_function_object_gc()));

        // 5. Let calleeRealm be F.[[Realm]].
        // 6. Set the Realm of calleeContext to calleeRealm.
        callee_context.realm.set(Some(function.realm()));

        // 7. Set the ScriptOrModule of calleeContext to null.
        // Note: This is already the default value.

        if function.function_environment_needed() {
            // 7. Let localEnv be NewFunctionEnvironment(F, newTarget).
            let native_javascript_backed_function = as_native_javascript_backed_function(function);
            let local_environment = new_function_environment_for_native_javascript_backed_function(
                vm,
                native_javascript_backed_function,
                None,
            );
            let shared_data = native_javascript_backed_function.shared_data();
            let function_environment_bindings_count = shared_data.function_environment_bindings_count();
            local_environment.set_environment_shape_cache(
                shared_data.function_environment_shape_cache(),
                function_environment_bindings_count,
            );
            local_environment.ensure_capacity(function_environment_bindings_count);

            // 8. Set the LexicalEnvironment of calleeContext to localEnv.
            callee_context.lexical_environment.set(Some(local_environment.upcast()));

            // 9. Set the VariableEnvironment of calleeContext to localEnv.
            callee_context
                .variable_environment
                .set(Some(local_environment.upcast()));
        } else {
            callee_context
                .lexical_environment
                .set(caller_context.lexical_environment.get());
            callee_context
                .variable_environment
                .set(caller_context.variable_environment.get());
        }

        // </8.> --------------------------------------------------------------------------

        // 9. Push calleeContext onto the execution context stack; calleeContext is now the running execution context.
        vm.push_execution_context_checking_stack_space(NonNull::from(callee_context))?;

        // 10. Let result be the Completion Record that is the result of evaluating F in a manner that conforms to the specification of F. The this value is uninitialized, argumentsList provides the named parameters, and newTarget provides the NewTarget value.
        let result = function.construct(vm, new_target);

        // 11. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
        vm.pop_execution_context();

        // 12. Return ? result.
        result
    }

    pub fn call(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        (self.methods().call)(self, vm)
    }

    pub fn construct(&self, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        (self.methods().construct)(self, vm, new_target)
    }

    pub fn function_environment_needed(&self) -> bool {
        (self.methods().function_environment_needed)(self)
    }

    pub fn function_environment_bindings_count(&self) -> usize {
        (self.methods().function_environment_bindings_count)(self)
    }

    pub fn name_for_call_stack(&self) -> Utf16String {
        Utf16String::from(&self.name())
    }

    pub fn name(&self) -> Utf16FlyString {
        self.name.get().unwrap_or_default()
    }

    pub fn realm(&self) -> Gc<Realm> {
        self.realm.get()
    }

    pub fn initial_name(&self) -> Option<Utf16FlyString> {
        self.initial_name.get()
    }

    pub fn set_initial_name(&self, initial_name: Utf16FlyString) {
        self.initial_name.set(Some(initial_name));
    }
}

impl FunctionObject {
    pub fn as_function_object_gc(&self) -> Gc<FunctionObject> {
        // SAFETY: Function objects only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }
}

/// The behaviour of a native function that captures state, the C++ AK::Function a CapturingNativeFunction calls.
pub trait NativeFunctionBehaviour: Trace + 'static {
    fn call(&self, vm: &Vm) -> ThrowCompletionOr<Value>;
}

/// A behaviour together with the state it captures. The behaviour is zero-sized, so the captures are all the state
/// there is, and tracing them keeps everything the behaviour can reach alive.
struct CapturingBehaviour<C, F> {
    captures: C,
    behaviour: F,
}

// SAFETY: The behaviour holds nothing, so the captures are all the cells this reaches.
unsafe impl<C: Trace, F> Trace for CapturingBehaviour<C, F> {
    fn trace(&self, visitor: &mut Visitor) {
        self.captures.trace(visitor);
    }
}

impl<C, F> NativeFunctionBehaviour for CapturingBehaviour<C, F>
where
    C: Trace + 'static,
    F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + 'static,
{
    fn call(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        (self.behaviour)(vm, &self.captures)
    }
}

/// A native function whose behaviour captures state.
#[repr(C)]
#[derive(Trace)]
struct CapturingNativeFunction {
    base: NativeFunction,
    native_function: OnceCell<Box<dyn NativeFunctionBehaviour>>,
}

static CAPTURING_NATIVE_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: CapturingNativeFunction::call,
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

static CAPTURING_NATIVE_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    native_function: Some(&CAPTURING_NATIVE_FUNCTION_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

define_cell!(
    CapturingNativeFunction,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: CAPTURING_NATIVE_FUNCTION_METHODS
);

impl CapturingNativeFunction {
    fn create<C, F>(vm: &Vm, base: NativeFunction, captures: C, behaviour: F) -> Gc<NativeFunction>
    where
        C: Trace + 'static,
        F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + 'static,
    {
        const {
            assert!(
                size_of::<F>() == 0,
                "a native function captures its state through its captures, not its behaviour"
            );
        };
        let function = allocate_object(
            vm,
            CapturingNativeFunction {
                base,
                native_function: OnceCell::new(),
            },
        );
        // The captures stay on the stack, where the collector finds them, until the function that keeps them alive
        // exists. Nothing allocates from the heap between allocating the function and storing them in it.
        let behaviour: Box<dyn NativeFunctionBehaviour> = Box::new(CapturingBehaviour { captures, behaviour });
        if function.native_function.set(behaviour).is_err() {
            unreachable!("the behaviour is stored once");
        }
        function.upcast()
    }

    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // SAFETY: Only CapturingNativeFunction has these methods.
        let function = unsafe { &*core::ptr::from_ref(function).cast::<CapturingNativeFunction>() };
        // NB: The behaviour is borrowed for the whole call. That is sound: the function is the [[Function]] of the
        //     running execution context, which keeps it and its behaviour alive, and the behaviour is never replaced.
        function
            .native_function
            .get()
            .expect("a capturing native function has its behaviour")
            .call(vm)
    }
}

impl Deref for CapturingNativeFunction {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

/// Defines the class of a NativeFunction subclass that is a constructor, as the C++ classes of the built-in
/// constructors override NativeFunction::call(), construct() and has_constructor(). The type keeps its NativeFunction in
/// a field named `base` and derefs to it.
macro_rules! define_native_function_class {
    ($type:ident, initialize: $initialize:expr, call: $call:expr, construct: $construct:expr) => {
        const _: () = {
            static VIRTUAL_METHODS: $crate::runtime::native_function::NativeFunctionMethods =
                $crate::runtime::native_function::NativeFunctionMethods {
                    call: $call,
                    construct: $construct,
                    ..$crate::runtime::native_function::NATIVE_FUNCTION_VIRTUAL_METHODS
                };
            static METHODS: $crate::runtime::object::ObjectMethods = $crate::runtime::object::ObjectMethods {
                initialize: $initialize,
                has_constructor: |_| true,
                native_function: Some(&VIRTUAL_METHODS),
                ..$crate::runtime::native_function::NATIVE_FUNCTION_METHODS
            };
            define_cell!(
                $type,
                Object,
                extends: [NativeFunction, FunctionObject, Object],
                methods: METHODS
            );
        };

        impl core::ops::Deref for $type {
            type Target = $crate::runtime::native_function::NativeFunction;

            fn deref(&self) -> &$crate::runtime::native_function::NativeFunction {
                &self.base
            }
        }
    };
}

pub(crate) use define_native_function_class;

impl RawNativeFunctionResult {
    pub fn from_completion(completion: ThrowCompletionOr<Value>) -> Self {
        match completion {
            Ok(value) => Self {
                payload: value.0,
                variant: 0,
            },
            Err(throw) => Self {
                payload: throw.value().0,
                variant: 1,
            },
        }
    }

    pub fn into_completion(self) -> ThrowCompletionOr<Value> {
        match self.variant {
            0 => Ok(Value(self.payload)),
            1 => Err(Throw::new(Value(self.payload))),
            variant => unreachable!("{variant} is not a ThrowCompletionOr variant"),
        }
    }
}

/// Calls a raw native function the way the interpreter does.
pub fn call_raw_native_function(function: RawNativeFunctionPointer, vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function.expect("a raw native function has a function pointer");
    let vm_pointer = core::ptr::from_ref(vm).cast_mut().cast();
    #[cfg(not(any(all(target_arch = "x86_64", target_vendor = "apple"), target_os = "windows")))]
    // SAFETY: Raw native functions take the VM they run on.
    let result = unsafe { function(vm_pointer) };
    #[cfg(any(all(target_arch = "x86_64", target_vendor = "apple"), target_os = "windows"))]
    let result = {
        let mut result = core::mem::MaybeUninit::<RawNativeFunctionResult>::uninit();
        // SAFETY: Raw native functions take the VM they run on and fill in the result.
        unsafe {
            function(result.as_mut_ptr(), vm_pointer);
            result.assume_init()
        }
    };
    result.into_completion()
}

/// Turns a `fn(&Vm) -> ThrowCompletionOr<Value>` into the RawNativeFunctionPointer the interpreter calls a raw native
/// function through, with the calling convention of the C++ runtime's NativeFunctionPointer on the target.
macro_rules! raw_native {
    ($function:expr) => {{
        #[cfg(not(any(all(target_arch = "x86_64", target_vendor = "apple"), target_os = "windows")))]
        unsafe extern "C" fn raw_native_thunk(
            vm: *mut core::ffi::c_void,
        ) -> $crate::layout_forward::RawNativeFunctionResult {
            // SAFETY: The interpreter and RawNativeFunction::call pass the VM the function runs on.
            let vm = unsafe { &*vm.cast::<$crate::interpreter::vm::Vm>() };
            let function: fn(
                &$crate::interpreter::vm::Vm,
            )
                -> $crate::runtime::completion::ThrowCompletionOr<$crate::layout::value::Value> = $function;
            $crate::layout_forward::RawNativeFunctionResult::from_completion(function(vm))
        }
        #[cfg(any(all(target_arch = "x86_64", target_vendor = "apple"), target_os = "windows"))]
        unsafe extern "C" fn raw_native_thunk(
            result: *mut $crate::layout_forward::RawNativeFunctionResult,
            vm: *mut core::ffi::c_void,
        ) {
            // SAFETY: The interpreter and RawNativeFunction::call pass the VM the function runs on.
            let vm = unsafe { &*vm.cast::<$crate::interpreter::vm::Vm>() };
            let function: fn(
                &$crate::interpreter::vm::Vm,
            )
                -> $crate::runtime::completion::ThrowCompletionOr<$crate::layout::value::Value> = $function;
            // SAFETY: The caller passes room for the result.
            unsafe {
                result.write($crate::layout_forward::RawNativeFunctionResult::from_completion(
                    function(vm),
                ))
            };
        }
        let pointer: $crate::layout_forward::RawNativeFunctionPointer = Some(raw_native_thunk);
        pointer
    }};
}

pub(crate) use raw_native;

pub static RAW_NATIVE_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: RawNativeFunction::call,
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

pub static RAW_NATIVE_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    native_function: Some(&RAW_NATIVE_FUNCTION_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

define_cell!(
    RawNativeFunction,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: RAW_NATIVE_FUNCTION_METHODS
);

// SAFETY: The index is a number; everything else is the NativeFunction's.
unsafe impl Trace for RawNativeFunction {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
    }
}

impl Deref for RawNativeFunction {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

impl RawNativeFunction {
    /// RawNativeFunction(NativeFunctionPointer, GC::Ptr<Object> prototype, Realm& realm, Optional<Bytecode::Builtin> builtin)
    pub fn new_with_realm(
        vm: &Vm,
        class: &'static Class,
        native_function: RawNativeFunctionPointer,
        prototype: Option<Gc<Object>>,
        realm: Gc<Realm>,
        builtin: Option<Builtin>,
    ) -> RawNativeFunction {
        let base = NativeFunction::new_with_realm(vm, class, prototype, realm, builtin);
        base.set_is_raw_native_function();
        RawNativeFunction {
            base,
            native_function_index: Cell::new(vm.register_native_function(NativeFunctionTableEntry {
                function: native_function,
                function_type: NativeFunctionType::RawNativeFunction,
            })),
        }
    }

    /// RawNativeFunction(Utf16FlyString name, NativeFunctionPointer, Object& prototype)
    pub fn new_with_name(
        vm: &Vm,
        class: &'static Class,
        name: Utf16FlyString,
        native_function: RawNativeFunctionPointer,
        prototype: Gc<Object>,
    ) -> RawNativeFunction {
        let base = NativeFunction::new_with_name(vm, class, name, prototype);
        base.set_is_raw_native_function();
        RawNativeFunction {
            base,
            native_function_index: Cell::new(vm.register_native_function(NativeFunctionTableEntry {
                function: native_function,
                function_type: NativeFunctionType::RawNativeFunction,
            })),
        }
    }

    // 10.3.3 CreateBuiltinFunction ( behaviour, length, name, additionalInternalSlotsList [ , realm [ , prototype [ , prefix ] ] ] ), https://tc39.es/ecma262/#sec-createbuiltinfunction
    pub fn create(
        vm: &Vm,
        behaviour: RawNativeFunctionPointer,
        length: i32,
        name: &PropertyKey,
        realm: Option<Gc<Realm>>,
        prefix: Option<&str>,
        builtin: Option<Builtin>,
    ) -> Gc<RawNativeFunction> {
        let realm = realm.unwrap_or_else(|| vm.current_realm().expect("there is a current realm"));

        let prototype = realm.function_prototype();
        let function = allocate_object(
            vm,
            Self::new_with_realm(vm, Self::CLASS, behaviour, Some(prototype), realm, builtin),
        );
        function.unsafe_set_shape(realm.native_function_shape());
        function.put_direct(realm.native_function_length_offset(), Value::from_i32(length));
        let function_name = function.make_function_name(vm, &ClassElementName::PropertyKey(name.clone()), prefix);
        function.put_direct(realm.native_function_name_offset(), Value::from_string(function_name));
        function
    }

    pub fn create_with_name(
        vm: &Vm,
        realm: Gc<Realm>,
        name: &Utf16FlyString,
        function: RawNativeFunctionPointer,
    ) -> Gc<RawNativeFunction> {
        allocate_object(
            vm,
            Self::new_with_name(vm, Self::CLASS, name.clone(), function, realm.function_prototype()),
        )
    }

    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        assert!(function.is::<RawNativeFunction>());
        // SAFETY: The function is a RawNativeFunction, which starts with its NativeFunction.
        let function = unsafe { &*core::ptr::from_ref(function).cast::<RawNativeFunction>() };
        call_raw_native_function(function.native_function(vm), vm)
    }

    pub fn native_function(&self, vm: &Vm) -> RawNativeFunctionPointer {
        vm.native_function(self.native_function_index.get(), NativeFunctionType::RawNativeFunction)
    }
}

/// Where a DirectGetterFunction finds the value it returns in a platform object.
#[derive(Clone, Copy, Debug, Default)]
pub struct DirectGetterConfiguration {
    // These are byte offsets to pointer-sized fields read directly by the interpreter. The bindings
    // generator obtains them from PlatformObject::wrapped_implementation_offset(), the implementation
    // field's generated offset helper, Wrappable::main_world_wrapper_offset(), and
    // GC::WeakImpl::value_offset(), respectively. DirectGetterFunction validates their alignment and
    // converts them to word offsets.
    pub wrapper_implementation_offset: usize,
    pub implementation_value_offset: usize,
    pub main_world_wrapper_offset: usize,
    pub weak_impl_value_offset: usize,
}

define_cell!(
    DirectGetterFunction,
    Object,
    extends: [RawNativeFunction, NativeFunction, FunctionObject, Object]
);

// SAFETY: The offsets are numbers; everything else is the RawNativeFunction's.
unsafe impl Trace for DirectGetterFunction {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
    }
}

impl Deref for DirectGetterFunction {
    type Target = RawNativeFunction;

    fn deref(&self) -> &RawNativeFunction {
        &self.base
    }
}

impl DirectGetterFunction {
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        behaviour: RawNativeFunctionPointer,
        length: i32,
        name: &PropertyKey,
        configuration: DirectGetterConfiguration,
        prefix: Option<&str>,
    ) -> Gc<DirectGetterFunction> {
        let function = allocate_object(
            vm,
            Self::new(vm, behaviour, realm.function_prototype(), realm, configuration),
        );
        function.unsafe_set_shape(realm.native_function_shape());
        function.put_direct(realm.native_function_length_offset(), Value::from_i32(length));
        let function_name = function.make_function_name(vm, &ClassElementName::PropertyKey(name.clone()), prefix);
        function.put_direct(realm.native_function_name_offset(), Value::from_string(function_name));
        function
    }

    fn new(
        vm: &Vm,
        native_function: RawNativeFunctionPointer,
        prototype: Gc<Object>,
        realm: Gc<Realm>,
        configuration: DirectGetterConfiguration,
    ) -> DirectGetterFunction {
        let base = RawNativeFunction::new_with_realm(vm, Self::CLASS, native_function, Some(prototype), realm, None);
        let word_size = size_of::<usize>();
        assert!(configuration.wrapper_implementation_offset.is_multiple_of(word_size));
        assert!(configuration.implementation_value_offset.is_multiple_of(word_size));
        assert!(configuration.main_world_wrapper_offset.is_multiple_of(word_size));
        assert!(configuration.weak_impl_value_offset.is_multiple_of(word_size));
        let word_offset =
            |offset: usize| u32::try_from(offset / word_size).expect("the word offset of a field fits in u32");
        let function = DirectGetterFunction {
            base,
            wrapper_implementation_word_offset: Cell::new(word_offset(configuration.wrapper_implementation_offset)),
            implementation_value_word_offset: Cell::new(word_offset(configuration.implementation_value_offset)),
            main_world_wrapper_word_offset: Cell::new(word_offset(configuration.main_world_wrapper_offset)),
            weak_impl_value_word_offset: Cell::new(word_offset(configuration.weak_impl_value_offset)),
        };
        function.set_is_direct_getter_function();
        function
    }
}
