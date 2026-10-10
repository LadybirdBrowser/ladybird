/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::{ControlFlow, Deref};
use core::ptr::NonNull;
use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::Executable;
use crate::gc::class::{Extends, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::run::should_dump_bytecode;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::environment::{Environment, PrivateEnvironment};
use crate::layout::execution_context::{ExecutionContext, ScriptOrModule};
pub use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::layout_forward::FlyStringSlot;
use crate::runtime::abstract_operations::{
    create_unmapped_arguments_object, get_prototype_from_constructor, new_function_environment,
};
use crate::runtime::async_function_driver_wrapper::AsyncFunctionDriverWrapper;
use crate::runtime::async_generator::AsyncGenerator;
use crate::runtime::class_field_definition::{ClassElementName, ClassFieldDefinition};
use crate::runtime::completion::{Must, Throw, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_environment::FunctionEnvironment;
use crate::runtime::function_object::{FUNCTION_OBJECT_METHODS, FunctionObject};
use crate::runtime::generator_object::{GeneratingFunction, GeneratorObject};
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ObjectMethods, PrivateElement, StackFrameInfo, allocate_object,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::shared_function_instance_data::{
    ClassFieldInitializerName, ConstructorKind, FunctionKind, SharedFunctionInstanceData, ThisMode,
};
use crate::source_code::SourceCode;
use crate::utf16::to_utf16_fly_string;

#[derive(Default, Trace)]
struct ClassData {
    fields: Vec<ClassFieldDefinition>,    // [[Fields]]
    private_methods: Vec<PrivateElement>, // [[PrivateMethods]]
}

/// The parts of an ECMAScript function object the interpreter does not read.
#[derive(Default, Trace)]
pub struct EcmascriptFunctionObjectStorage {
    class_data: GcRefCell<Option<Box<ClassData>>>,
    may_need_lazy_prototype_instantiation: Cell<bool>,
    is_method: Cell<bool>,
}

pub static ECMASCRIPT_FUNCTION_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    internal_get_own_property: EcmascriptFunctionObject::internal_get_own_property,
    // NB: Functions answer for "prototype" before instantiating it lazily, and for "caller" and "arguments" if they
    //     support the legacy properties, whatever their shape holds. They answer for other names like ordinary
    //     objects.
    is_cacheable_for_absence_of: |_, vm, property_key| {
        *property_key != vm.names.prototype && *property_key != vm.names.caller && *property_key != vm.names.arguments
    },
    internal_own_property_keys: EcmascriptFunctionObject::internal_own_property_keys,
    internal_call: Some(EcmascriptFunctionObject::internal_call),
    internal_construct: Some(EcmascriptFunctionObject::internal_construct),
    has_constructor: |object| as_ecmascript_function(object).has_constructor(),
    is_strict_mode: |object| as_ecmascript_function(object).shared_data().strict(),
    get_stack_frame_info: EcmascriptFunctionObject::get_stack_frame_info,
    function_realm: |object| Some(object.shape().realm()),
    name_for_call_stack: |object| as_ecmascript_function(object).name_for_call_stack(),
    ..FUNCTION_OBJECT_METHODS
};

define_cell!(
    EcmascriptFunctionObject,
    Object,
    extends: [FunctionObject, Object],
    methods: ECMASCRIPT_FUNCTION_OBJECT_METHODS
);

// SAFETY: Visits every cell an ECMAScript function object holds, besides those of its FunctionObject.
unsafe impl Trace for EcmascriptFunctionObject {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
        self.environment.trace(visitor);
        self.private_environment.trace(visitor);
        self.home_object.trace(visitor);
        self.name_string.trace(visitor);
        self.shared_data.trace(visitor);
        self.storage.trace(visitor);
        match self.script_or_module.get() {
            ScriptOrModule::Empty => {}
            ScriptOrModule::Script(script) => script.trace(visitor),
            ScriptOrModule::Module(module) => module.trace(visitor),
        }
    }
}

impl Deref for EcmascriptFunctionObject {
    type Target = FunctionObject;

    fn deref(&self) -> &FunctionObject {
        &self.base
    }
}

/// The function an internal method of an ECMAScript function object was called on.
fn as_ecmascript_function(object: &Object) -> &EcmascriptFunctionObject {
    assert!(object.is_ecmascript_function_object());
    // SAFETY: Only ECMAScript function objects have the flag, and they start with their Object.
    unsafe { &*core::ptr::from_ref(object).cast::<EcmascriptFunctionObject>() }
}

/// The object as an ECMAScript function object, if its flag says that it is one.
pub fn as_ecmascript_function_object<T: Extends<Object>>(cell: Gc<T>) -> Option<Gc<EcmascriptFunctionObject>> {
    let object = cell.upcast::<Object>();
    object
        .is_ecmascript_function_object()
        // SAFETY: Only ECMAScript function objects have the flag.
        .then(|| unsafe { Gc::from_non_null(object.as_non_null().cast()) })
}

/// Value::as_if<ECMAScriptFunctionObject>().
pub fn value_as_ecmascript_function_object(value: Value) -> Option<Gc<EcmascriptFunctionObject>> {
    if !value.is_object() {
        return None;
    }
    as_ecmascript_function_object(value.as_object())
}

fn prototype_for_function_kind(realm: Gc<Realm>, kind: FunctionKind) -> Gc<Object> {
    match kind {
        FunctionKind::Normal => realm.function_prototype(),
        FunctionKind::Generator => realm.generator_function_prototype(),
        FunctionKind::Async => realm.async_function_prototype(),
        FunctionKind::AsyncGenerator => realm.async_generator_function_prototype(),
    }
}

impl EcmascriptFunctionObject {
    pub fn create_from_function_data_with_prototype(
        vm: &Vm,
        realm: Gc<Realm>,
        shared_data: Gc<SharedFunctionInstanceData>,
        parent_environment: Option<Gc<Environment>>,
        private_environment: Option<Gc<PrivateEnvironment>>,
        prototype: Gc<Object>,
    ) -> Gc<EcmascriptFunctionObject> {
        let function = allocate_object(
            vm,
            Self::new(vm, shared_data, parent_environment, private_environment, prototype),
        );
        function.initialize(vm, realm);
        function
    }

    pub fn create_from_function_data(
        vm: &Vm,
        realm: Gc<Realm>,
        shared_data: Gc<SharedFunctionInstanceData>,
        parent_environment: Option<Gc<Environment>>,
        private_environment: Option<Gc<PrivateEnvironment>>,
    ) -> Gc<EcmascriptFunctionObject> {
        let prototype = prototype_for_function_kind(realm, shared_data.kind());
        Self::create_from_function_data_with_prototype(
            vm,
            realm,
            shared_data,
            parent_environment,
            private_environment,
            prototype,
        )
    }

    fn new(
        vm: &Vm,
        shared_data: Gc<SharedFunctionInstanceData>,
        parent_environment: Option<Gc<Environment>>,
        private_environment: Option<Gc<PrivateEnvironment>>,
        prototype: Gc<Object>,
    ) -> Self {
        let base =
            FunctionObject::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No);
        base.set_is_ecmascript_function_object();

        // OPTIMIZATION: Start from a premade shape that already has this function kind's own properties in spec order.
        //               Arrow functions use the same shape as other functions of their kind, but never get a lazy prototype.
        let realm = base.shape().realm();
        let function_shape = match shared_data.kind() {
            FunctionKind::Normal => realm.normal_function_shape(),
            FunctionKind::Generator => realm.generator_function_shape(),
            FunctionKind::Async => realm.async_function_shape(),
            FunctionKind::AsyncGenerator => realm.async_generator_function_shape(),
        };
        // NB: unsafe_set_shape(); the named storage gets room for the shape's properties when the function is allocated.
        if function_shape.prototype() == Some(prototype) {
            base.shape.set(function_shape);
        } else {
            base.shape
                .set(function_shape.create_prototype_transition(vm, Some(prototype)));
        }

        // 15. Set F.[[ScriptOrModule]] to GetActiveScriptOrModule().
        let script_or_module = vm.get_active_script_or_module();

        Self {
            base,
            shared_data: Cell::new(shared_data),
            name: FlyStringSlot::new(None),
            name_string: Cell::new(None),
            environment: Cell::new(parent_environment),
            private_environment: Cell::new(private_environment),
            script_or_module: Cell::new(script_or_module),
            home_object: Cell::new(None),
            storage: EcmascriptFunctionObjectStorage::default(),
        }
    }

    fn initialize(&self, vm: &Vm, realm: Gc<Realm>) {
        // Note: The ordering of these properties must be: length, name, prototype which is the order
        //       they are defined in the spec: https://tc39.es/ecma262/#sec-function-instances .
        //       This is observable through something like: https://tc39.es/ecma262/#sec-ordinaryownpropertykeys
        //       which must give the properties in chronological order which in this case is the order they
        //       are defined in the spec.

        let name_string = PrimitiveString::create_from_fly_string(vm, &self.name());
        self.name_string.set(Some(name_string));

        // NOTE: The constructor gave us a premade shape with "length" and "name" (and "prototype" for generator kinds) at
        //       these offsets, with the attributes the spec requires, so we only have to store the values.
        self.put_direct(
            realm.normal_function_length_offset(),
            Value::from_i32(self.function_length()),
        );
        self.put_direct(realm.normal_function_name_offset(), Value::from_string(name_string));

        match self.kind() {
            FunctionKind::Normal => {
                if !self.is_arrow_function() {
                    self.storage.may_need_lazy_prototype_instantiation.set(true);
                }
            }
            FunctionKind::Generator => {
                // prototype is "g1.prototype" in figure-2 (https://tc39.es/ecma262/img/figure-2.png)
                self.put_direct(
                    realm.generator_function_prototype_property_offset(),
                    Value::from_object(Object::create_prototype(
                        vm,
                        realm,
                        Some(realm.generator_function_prototype_prototype()),
                    )),
                );
            }
            FunctionKind::Async => {
                // 27.7.4 AsyncFunction Instances, https://tc39.es/ecma262/#sec-async-function-instances
                // AsyncFunction instances do not have a prototype property as they are not constructible.
            }
            FunctionKind::AsyncGenerator => {
                self.put_direct(
                    realm.generator_function_prototype_property_offset(),
                    Value::from_object(Object::create_prototype(
                        vm,
                        realm,
                        Some(realm.async_generator_function_prototype_prototype()),
                    )),
                );
            }
        }
    }

    pub fn as_ecmascript_function_gc(&self) -> Gc<EcmascriptFunctionObject> {
        // SAFETY: ECMAScript function objects only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }

    /// The function's executable, compiled first if the function never ran.
    pub fn compiled_executable(&self, vm: &Vm) -> Gc<Executable> {
        let shared_data = self.shared_data();
        if let Some(executable) = shared_data.executable() {
            return executable;
        }
        let rust_executable = SharedFunctionInstanceData::compile_function(vm, shared_data, false)
            .expect("an ECMAScript function compiles to an executable");
        shared_data.set_executable(Some(rust_executable));
        rust_executable.set_name(self.name());
        if should_dump_bytecode() {
            rust_executable.dump();
        }
        shared_data.clear_compile_inputs();
        rust_executable
    }

    fn get_stack_frame_info(object: &Object, vm: &Vm, stack_frame_info: &mut StackFrameInfo) {
        let function = as_ecmascript_function(object);
        let executable = function.compiled_executable(vm);
        stack_frame_info.registers_and_locals_count = executable.registers_and_locals_count();
        stack_frame_info.constant_count =
            u32::try_from(executable.constants().len()).expect("the constant count fits in u32");
        stack_frame_info.argument_count = stack_frame_info.argument_count.max(function.formal_parameter_count());
    }

    // 10.2.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-call-thisargument-argumentslist
    fn internal_call(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        this_argument: Value,
    ) -> ThrowCompletionOr<Value> {
        let function = as_ecmascript_function(object);

        debug_assert!(function.bytecode_executable().is_some());

        // 1. Let callerContext be the running execution context.
        // NOTE: No-op, kept by the VM in its execution context stack.

        // 2. Let calleeContext be PrepareForOrdinaryCall(F, undefined).
        function.prepare_for_ordinary_call(vm, callee_context, None);

        // 3. Assert: calleeContext is now the running execution context.
        debug_assert!(vm.running_execution_context() == Some(NonNull::from(callee_context)));

        // 4. If F.[[IsClassConstructor]] is true, then
        if function.is_class_constructor() {
            // a. Let error be a newly created TypeError object.
            // b. NOTE: error is created in calleeContext with F's associated Realm Record.
            let throw_completion = vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::ClassConstructorWithoutNew,
                &[&function.name()],
            );

            // c. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
            vm.pop_execution_context();

            // d. Return ThrowCompletion(error).
            return throw_completion;
        }

        // 5. Perform OrdinaryCallBindThis(F, calleeContext, thisArgument).
        if function.uses_this() {
            function.ordinary_call_bind_this(vm, callee_context, this_argument);
        }

        // 6. Let result be Completion(OrdinaryCallEvaluateBody(F, argumentsList)).
        let result = function.ordinary_call_evaluate_body(vm, callee_context);

        // 7. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
        vm.pop_execution_context();

        // 8. If result.[[Type]] is return, return result.[[Value]].
        // 9. Assert: result is a throw completion.
        // 10. Return ? result.
        result
    }

    // 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
    fn internal_construct(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        new_target: Gc<FunctionObject>,
    ) -> ThrowCompletionOr<Gc<Object>> {
        let function = as_ecmascript_function(object);

        debug_assert!(function.bytecode_executable().is_some());

        // 1. Let callerContext be the running execution context.
        // NOTE: No-op, kept by the VM in its execution context stack.

        // 2. Let kind be F.[[ConstructorKind]].
        let kind = function.constructor_kind();

        let mut this_argument: Option<Gc<Object>> = None;

        // 3. If kind is base, then
        if kind == ConstructorKind::Base {
            // a. Let thisArgument be ? OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%").
            // NB: With room for the properties the constructor is known to add.
            let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::object_prototype)?;
            this_argument = Some(Object::create_for_construct(vm, prototype, function.shared_data()));
        }

        // 4. Let calleeContext be PrepareForOrdinaryCall(F, newTarget).
        function.prepare_for_ordinary_call(vm, callee_context, Some(new_target.upcast()));

        // 5. Assert: calleeContext is now the running execution context.
        debug_assert!(vm.running_execution_context() == Some(NonNull::from(callee_context)));

        // 6. If kind is base, then
        if kind == ConstructorKind::Base {
            let this_argument = this_argument.expect("a base constructor has a this argument");

            // a. Perform OrdinaryCallBindThis(F, calleeContext, thisArgument).
            if function.uses_this() {
                function.ordinary_call_bind_this(vm, callee_context, Value::from_object(this_argument));
            }

            // b. Let initializeResult be Completion(InitializeInstanceElements(thisArgument, F)).
            let initialize_result =
                this_argument.initialize_instance_elements(vm, function.as_ecmascript_function_gc());

            // c. If initializeResult is an abrupt completion, then
            if let Err(throw) = initialize_result {
                // i. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
                vm.pop_execution_context();

                // ii. Return ? initializeResult.
                return Err(throw);
            }
        }

        // 7. Let constructorEnv be the LexicalEnvironment of calleeContext.
        let constructor_env = callee_context.lexical_environment.get();

        // 8. Let result be Completion(OrdinaryCallEvaluateBody(F, argumentsList)).
        let result = function.ordinary_call_evaluate_body(vm, callee_context);

        // 9. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
        vm.pop_execution_context();

        // 10. If result is a throw completion, then
        //     a. Return ? result.
        let result = result?;

        // 11. Assert: result is a return completion.
        // NOTE: We already checked !is_error() above.

        // 12. If Type(result.[[Value]]) is Object, return result.[[Value]].
        if result.is_object() {
            return Ok(result.as_object());
        }

        // 13. If kind is base, return thisArgument.
        if kind == ConstructorKind::Base {
            return Ok(this_argument.expect("a base constructor has a this argument"));
        }

        // 14. If result.[[Value]] is not undefined, throw a TypeError exception.
        if !result.is_undefined() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::DerivedConstructorReturningInvalidValue,
                &[],
            );
        }

        // 15. Let thisBinding be ? constructorEnv.GetThisBinding().
        let this_binding = constructor_env
            .expect("a constructor runs in an environment")
            .get_this_binding(vm)?;

        // 16. Assert: Type(thisBinding) is Object.
        debug_assert!(this_binding.is_object());

        // 17. Return thisBinding.
        Ok(this_binding.as_object())
    }

    // 10.2.7 MakeMethod ( F, homeObject ), https://tc39.es/ecma262/#sec-makemethod
    pub fn make_method(&self, home_object: Gc<Object>) {
        // 1. Set F.[[HomeObject]] to homeObject.
        self.home_object.set(Some(home_object));
        self.storage.is_method.set(true);
        self.storage.may_need_lazy_prototype_instantiation.set(false);

        // 2. Return unused.
    }

    // 10.2.1.1 PrepareForOrdinaryCall ( F, newTarget ), https://tc39.es/ecma262/#sec-prepareforordinarycall
    pub fn prepare_for_ordinary_call(
        &self,
        vm: &Vm,
        callee_context: &ExecutionContext,
        new_target: Option<Gc<Object>>,
    ) {
        // 1. Let callerContext be the running execution context.
        // 2. Let calleeContext be a new ECMAScript code execution context.

        // 3. Set the Function of calleeContext to F.
        callee_context.function.set(Some(self.as_function_object_gc()));

        // 4. Let calleeRealm be F.[[Realm]].
        // 5. Set the Realm of calleeContext to calleeRealm.
        callee_context.realm.set(self.realm());

        // 6. Set the ScriptOrModule of calleeContext to F.[[ScriptOrModule]].
        callee_context.script_or_module.set(self.script_or_module.get());

        if self.function_environment_needed() {
            // 7. Let localEnv be NewFunctionEnvironment(F, newTarget).
            let local_environment = new_function_environment(vm, self.as_ecmascript_function_gc(), new_target);
            let shared_data = self.shared_data();
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
            callee_context.lexical_environment.set(self.environment());
            callee_context.variable_environment.set(self.environment());
        }

        // 10. Set the PrivateEnvironment of calleeContext to F.[[PrivateEnvironment]].
        callee_context.private_environment.set(self.private_environment.get());

        // 11. If callerContext is not already suspended, suspend callerContext.
        // 12. Push calleeContext onto the execution context stack; calleeContext is now the running execution context.

        // NOTE: We don't check for stack overflow here. The bytecode interpreter will do it anyway
        //       when entering the function we're about to call.
        vm.push_execution_context(NonNull::from(callee_context));

        // 13. NOTE: Any exception objects produced after this point are associated with calleeRealm.
        // 14. Return calleeContext.
        // NOTE: The caller allocated calleeContext, so there is nothing to return.
    }

    // 10.2.1.2 OrdinaryCallBindThis ( F, calleeContext, thisArgument ), https://tc39.es/ecma262/#sec-ordinarycallbindthis
    pub fn ordinary_call_bind_this(&self, vm: &Vm, callee_context: &ExecutionContext, this_argument: Value) {
        // 1. Let thisMode be F.[[ThisMode]].
        // If thisMode is lexical, return unused.
        if self.this_mode() == ThisMode::Lexical {
            return;
        }

        // 3. Let calleeRealm be F.[[Realm]].
        let callee_realm = self.realm().expect("an ECMAScript function has a realm");

        // 4. Let localEnv be the LexicalEnvironment of calleeContext.
        let local_env = callee_context.lexical_environment.get();

        // 5. If thisMode is strict, let thisValue be thisArgument.
        let this_value = if self.this_mode() == ThisMode::Strict {
            this_argument
        }
        // 6. Else,
        else {
            // a. If thisArgument is undefined or null, then
            if this_argument.is_nullish() {
                // i. Let globalEnv be calleeRealm.[[GlobalEnv]].
                // ii. Assert: globalEnv is a global Environment Record.
                // iii. Let thisValue be globalEnv.[[GlobalThisValue]].
                Value::from_object(callee_realm.global_environment().global_this_value())
            }
            // b. Else,
            else {
                // i. Let thisValue be ! ToObject(thisArgument).
                let this_value = Value::from_object(this_argument.to_object(vm).must());

                // ii. NOTE: ToObject produces wrapper objects using calleeRealm.
                debug_assert!(vm.current_realm() == Some(callee_realm));

                this_value
            }
        };

        // 7. Assert: localEnv is a function Environment Record.
        // 8. Assert: The next step never returns an abrupt completion because localEnv.[[ThisBindingStatus]] is not initialized.
        // 9. Perform ! localEnv.BindThisValue(thisValue).
        callee_context.this_value.set(this_value);
        if self.function_environment_needed() {
            local_env
                .and_then(|environment| environment.downcast::<FunctionEnvironment>())
                .expect("the lexical environment of the call is a function environment")
                .bind_this_value(vm, this_value)
                .must();
        }

        // 10. Return unused.
    }

    // 10.2.1.4 OrdinaryCallEvaluateBody ( F, argumentsList ), https://tc39.es/ecma262/#sec-ordinarycallevaluatebody
    // 15.8.4 Runtime Semantics: EvaluateAsyncFunctionBody, https://tc39.es/ecma262/#sec-runtime-semantics-evaluatefunctionbody
    fn ordinary_call_evaluate_body(&self, vm: &Vm, context: &ExecutionContext) -> ThrowCompletionOr<Value> {
        let executable = self
            .bytecode_executable()
            .expect("a function is compiled before it is called");
        let result = vm
            .run_executable(NonNull::from(context), executable, 0)
            .map_err(Throw::new)?;

        // NOTE: Running the bytecode should eventually return a completion.
        // Until it does, we assume "return" and include the undefined fallback from the call site.
        if self.kind() == FunctionKind::Normal {
            return Ok(result);
        }

        let realm = context.realm.get().expect("a function runs in a realm");
        if self.kind() == FunctionKind::AsyncGenerator {
            return Ok(Value::from_object(AsyncGenerator::create(
                vm,
                realm,
                GeneratingFunction::Ecmascript(self.as_ecmascript_function_gc()),
                context.copy(),
            )));
        }

        let generator_object = GeneratorObject::create(
            vm,
            realm,
            GeneratingFunction::Ecmascript(self.as_ecmascript_function_gc()),
            context.copy(),
        );

        // NOTE: Async functions are entirely transformed to generator functions, and wrapped in a custom driver that returns a promise.
        if self.kind() == FunctionKind::Async {
            return Ok(Value::from_object(AsyncFunctionDriverWrapper::create(
                vm,
                realm,
                generator_object,
            )));
        }

        debug_assert!(self.kind() == FunctionKind::Generator);
        Ok(Value::from_object(generator_object))
    }

    pub fn set_name(&self, vm: &Vm, name: &Utf16FlyString) {
        self.name.set(Some(name.clone()));
        let name_string = PrimitiveString::create_from_fly_string(vm, name);
        self.name_string.set(Some(name_string));
        let mut descriptor = PropertyDescriptor {
            value: Some(Value::from_string(name_string)),
            writable: Some(false),
            enumerable: Some(false),
            configurable: Some(true),
            ..Default::default()
        };
        self.define_property_or_throw(vm, &vm.names.name, &mut descriptor)
            .must();
    }

    pub fn set_inferred_name(&self, vm: &Vm, name: &ClassElementName, prefix: Option<&str>) {
        let function_name = self.make_function_name(vm, name, prefix);
        self.set_name(vm, &to_utf16_fly_string(&function_name.utf16_string()));
    }

    pub fn name_for_call_stack(&self) -> Utf16String {
        self.name_string
            .get()
            .expect("an ECMAScript function has its name string once initialized")
            .utf16_string()
    }

    pub fn name(&self) -> Utf16FlyString {
        self.name.get().unwrap_or_else(|| self.shared_data().name())
    }

    fn ensure_class_data(&self) {
        let mut class_data = self.storage.class_data.borrow_mut();
        if class_data.is_none() {
            *class_data = Some(Box::default());
        }
    }

    pub fn has_class_data(&self) -> bool {
        self.storage.class_data.borrow().is_some()
    }

    pub fn fields_count(&self) -> usize {
        self.storage
            .class_data
            .borrow()
            .as_ref()
            .map_or(0, |class_data| class_data.fields.len())
    }

    /// A copy of the field record at `index` of [[Fields]].
    pub fn field(&self, index: usize) -> ClassFieldDefinition {
        self.storage
            .class_data
            .borrow()
            .as_ref()
            .expect("the function has class data")
            .fields[index]
            .clone()
    }

    pub fn add_field(&self, field: ClassFieldDefinition) {
        self.ensure_class_data();
        self.storage
            .class_data
            .borrow_mut()
            .as_mut()
            .expect("the function has class data")
            .fields
            .push(field);
    }

    pub fn private_methods_count(&self) -> usize {
        self.storage
            .class_data
            .borrow()
            .as_ref()
            .map_or(0, |class_data| class_data.private_methods.len())
    }

    /// A copy of the element at `index` of [[PrivateMethods]].
    pub fn private_method(&self, index: usize) -> PrivateElement {
        self.storage
            .class_data
            .borrow()
            .as_ref()
            .expect("the function has class data")
            .private_methods[index]
            .clone()
    }

    pub fn add_private_method(&self, method: PrivateElement) {
        self.ensure_class_data();
        self.storage
            .class_data
            .borrow_mut()
            .as_mut()
            .expect("the function has class data")
            .private_methods
            .push(method);
    }

    pub fn shared_data(&self) -> Gc<SharedFunctionInstanceData> {
        self.shared_data.get()
    }

    pub fn is_module_wrapper(&self) -> bool {
        self.shared_data().is_module_wrapper()
    }

    pub fn set_is_module_wrapper(&self, is_module_wrapper: bool) {
        self.shared_data().set_is_module_wrapper(is_module_wrapper);
    }

    pub fn formal_parameter_count(&self) -> u32 {
        self.shared_data().formal_parameter_count()
    }

    /// Shares the cached parameter map with an arguments object.
    pub fn mapped_argument_names(&self) -> Rc<[Utf16FlyString]> {
        self.shared_data().mapped_argument_names()
    }

    pub fn parameter_binding_names(&self) -> Rc<[Utf16FlyString]> {
        self.shared_data().parameter_binding_names()
    }

    pub fn set_is_class_constructor(&self) {
        self.shared_data().set_is_class_constructor();
    }

    pub fn bytecode_executable(&self) -> Option<Gc<Executable>> {
        self.shared_data().executable()
    }

    pub fn can_inline_call(&self) -> bool {
        self.shared_data().can_inline_call()
    }

    pub fn inline_call_executable(&self) -> Gc<Executable> {
        assert!(self.can_inline_call());
        self.shared_data()
            .executable()
            .expect("a function that can be called inline has an executable")
    }

    pub fn environment(&self) -> Option<Gc<Environment>> {
        self.environment.get()
    }

    pub fn private_environment(&self) -> Option<Gc<PrivateEnvironment>> {
        self.private_environment.get()
    }

    pub fn script_or_module(&self) -> ScriptOrModule {
        self.script_or_module.get()
    }

    pub fn constructor_kind(&self) -> ConstructorKind {
        self.shared_data().constructor_kind()
    }

    pub fn set_constructor_kind(&self, constructor_kind: ConstructorKind) {
        self.shared_data().set_constructor_kind(constructor_kind);
    }

    pub fn this_mode(&self) -> ThisMode {
        self.shared_data().this_mode()
    }

    pub fn is_arrow_function(&self) -> bool {
        self.shared_data().is_arrow_function()
    }

    pub fn is_class_constructor(&self) -> bool {
        self.shared_data().is_class_constructor()
    }

    pub fn uses_this(&self) -> bool {
        self.shared_data().uses_this()
    }

    pub fn this_value_needs_environment_resolution(&self) -> bool {
        self.shared_data().this_value_needs_environment_resolution()
    }

    pub fn function_length(&self) -> i32 {
        self.shared_data().function_length()
    }

    pub fn home_object(&self) -> Option<Gc<Object>> {
        self.home_object.get()
    }

    pub fn set_home_object(&self, home_object: Option<Gc<Object>>) {
        self.home_object.set(home_object);
    }

    pub fn source_text(&self) -> Utf16String {
        self.shared_data().source_text()
    }

    pub fn set_source_text(&self, source_text: Utf16String) {
        self.shared_data().set_source_text(source_text);
    }

    pub fn set_source_text_range(
        &self,
        source_code: &Rc<SourceCode>,
        source_text_offset: usize,
        source_text_length: usize,
    ) {
        self.shared_data()
            .set_source_text_range(source_code, source_text_offset, source_text_length);
    }

    // This is for IsSimpleParameterList (static semantics)
    pub fn has_simple_parameter_list(&self) -> bool {
        self.shared_data().has_simple_parameter_list()
    }

    // Equivalent to absence of [[Construct]]
    pub fn has_constructor(&self) -> bool {
        self.kind() == FunctionKind::Normal && !self.is_arrow_function() && !self.storage.is_method.get()
    }

    pub fn kind(&self) -> FunctionKind {
        self.shared_data().kind()
    }

    // This is used by LibWeb to disassociate event handler attribute callback functions from the nearest script on the call stack.
    // https://html.spec.whatwg.org/multipage/webappapis.html#getting-the-current-value-of-the-event-handler Step 3.11
    pub fn set_script_or_module(&self, script_or_module: ScriptOrModule) {
        self.script_or_module.set(script_or_module);
    }

    pub fn class_field_initializer_name(&self) -> ClassFieldInitializerName {
        self.shared_data().class_field_initializer_name()
    }

    pub fn allocates_function_environment(&self) -> bool {
        self.shared_data().function_environment_needed()
    }

    pub fn function_environment_needed(&self) -> bool {
        self.shared_data().function_environment_needed()
    }

    fn supports_legacy_caller_or_arguments(&self) -> bool {
        // https://tc39.es/ecma262/#sec-forbidden-extensions
        //
        // ECMAScript function objects defined using syntactic constructors in strict mode code must not be created with own
        // properties named *"caller"* or *"arguments"*. Such own properties also must not be created for function objects
        // defined using an |ArrowFunction|, |MethodDefinition|, |GeneratorDeclaration|, |GeneratorExpression|,
        // |AsyncGeneratorDeclaration|, |AsyncGeneratorExpression|, |ClassDeclaration|, |ClassExpression|,
        // |AsyncFunctionDeclaration|, |AsyncFunctionExpression|, or |AsyncArrowFunction| regardless of whether the definition
        // is contained in strict mode code.
        // Built-in functions, strict functions created using the Function constructor, generator functions created using
        // the Generator constructor, async functions created using the AsyncFunction constructor, and functions created
        // using the `bind` method also must not be created with such own properties.
        self.kind() == FunctionKind::Normal
            && !self.is_arrow_function()
            && !self.is_class_constructor()
            && !self.storage.is_method.get()
            && !self.is_strict_mode()
    }

    fn legacy_caller(&self, vm: &Vm) -> Value {
        let this_function = self.as_function_object_gc();
        let mut caller: Option<Gc<EcmascriptFunctionObject>> = None;
        let mut found_this_function = false;

        vm.for_each_execution_context_top_to_bottom(|context| {
            if !found_this_function {
                if context.function.get() == Some(this_function) {
                    found_this_function = true;
                }
                return ControlFlow::Continue(());
            }

            let Some(function) = context.function.get() else {
                return ControlFlow::Continue(());
            };

            caller = as_ecmascript_function_object(function);
            ControlFlow::Break(())
        });

        // https://tc39.es/ecma262/#sec-forbidden-extensions
        //
        // If an implementation extends any function object with an own property named *"caller"* the value of that property,
        // as observed using [[Get]] or [[GetOwnProperty]], must not be a strict function object. If it is an accessor
        // property, the function that is the value of the property's [[Get]] attribute must never return a strict function
        // when called.
        match caller {
            Some(caller) if caller.supports_legacy_caller_or_arguments() => Value::from_object(caller),
            _ => Value::NULL,
        }
    }

    fn legacy_arguments(&self, vm: &Vm) -> Value {
        let this_function = self.as_function_object_gc();
        let mut active_context: Option<NonNull<ExecutionContext>> = None;

        vm.for_each_execution_context_top_to_bottom(|context| {
            if context.function.get() != Some(this_function) {
                return ControlFlow::Continue(());
            }

            active_context = Some(NonNull::from(context));
            ControlFlow::Break(())
        });

        let Some(active_context) = active_context else {
            return Value::NULL;
        };

        // SAFETY: The context is a frame of a call of this function that has not returned yet.
        let active_context = unsafe { active_context.as_ref() };
        let arguments = active_context.arguments();
        let passed_arguments = &arguments[..active_context.passed_argument_count.get() as usize];
        let arguments_object = create_unmapped_arguments_object(vm, passed_arguments);
        if self.has_simple_parameter_list() {
            arguments_object.define_direct_property(
                vm,
                &vm.names.callee,
                Value::from_object(self.as_function_object_gc()),
                PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            );
        }
        Value::from_object(arguments_object)
    }

    fn internal_get_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        let function = as_ecmascript_function(object);

        if function.supports_legacy_caller_or_arguments() {
            let descriptor = object.ordinary_get_own_property(vm, property_key)?;
            if descriptor.is_some() {
                return Ok(descriptor);
            }

            if *property_key == vm.names.caller {
                return Ok(Some(PropertyDescriptor {
                    value: Some(function.legacy_caller(vm)),
                    writable: Some(false),
                    enumerable: Some(false),
                    configurable: Some(false),
                    ..Default::default()
                }));
            }
            if *property_key == vm.names.arguments {
                return Ok(Some(PropertyDescriptor {
                    value: Some(function.legacy_arguments(vm)),
                    writable: Some(false),
                    enumerable: Some(false),
                    configurable: Some(false),
                    ..Default::default()
                }));
            }
        }

        if function.storage.may_need_lazy_prototype_instantiation.get() && *property_key == vm.names.prototype {
            let realm = function.realm().expect("an ECMAScript function has a realm");
            let metadata = object.shape().lookup(property_key);
            if metadata.is_none() {
                let prototype = Object::create_with_premade_shape(vm, realm.normal_function_prototype_shape());
                prototype.put_direct(
                    realm.normal_function_prototype_constructor_offset(),
                    Value::from_object(function.as_function_object_gc()),
                );
                object.define_direct_property(
                    vm,
                    &vm.names.prototype,
                    Value::from_object(prototype),
                    PropertyAttributes::new(Attribute::WRITABLE),
                );
            }
            function.storage.may_need_lazy_prototype_instantiation.set(false);
        }

        object.ordinary_get_own_property(vm, property_key)
    }

    fn internal_own_property_keys<'vm>(object: &Object, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        let function = as_ecmascript_function(object);

        if function.storage.may_need_lazy_prototype_instantiation.get() {
            Self::internal_get_own_property(object, vm, &vm.names.prototype)?;
        }

        let keys = object.ordinary_own_property_keys(vm)?;
        if !function.supports_legacy_caller_or_arguments() {
            return Ok(keys);
        }

        let mut insertion_index = keys.len();
        let name = Utf16String::from(vm.names.name.as_string());
        for index in 0..keys.len() {
            let key = keys.get(index).expect("the index is in bounds");
            if key.is_string() && key.as_string().utf16_string() == name {
                insertion_index = index + 1;
                break;
            }
        }

        if object.ordinary_get_own_property(vm, &vm.names.arguments)?.is_none() {
            keys.insert(
                insertion_index,
                Value::from_string(PrimitiveString::create_from_fly_string(
                    vm,
                    vm.names.arguments.as_string(),
                )),
            );
            insertion_index += 1;
        }
        if object.ordinary_get_own_property(vm, &vm.names.caller)?.is_none() {
            keys.insert(
                insertion_index,
                Value::from_string(PrimitiveString::create_from_fly_string(vm, vm.names.caller.as_string())),
            );
        }

        Ok(keys)
    }
}
