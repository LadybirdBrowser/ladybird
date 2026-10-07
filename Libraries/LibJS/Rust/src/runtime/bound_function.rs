/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell};
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::{FUNCTION_OBJECT_METHODS, FunctionObject};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ObjectMethods, StackFrameInfo, allocate_object};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct BoundFunction {
    base: FunctionObject,
    bound_target_function: Cell<Gc<FunctionObject>>, // [[BoundTargetFunction]]
    bound_this: Cell<Value>,                         // [[BoundThis]]
    bound_arguments: OnceCell<Box<[Value]>>,         // [[BoundArguments]]
}

pub static BOUND_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    internal_call: Some(BoundFunction::internal_call),
    internal_construct: Some(BoundFunction::internal_construct),
    is_strict_mode: |object| as_bound_function(object).bound_target_function().is_strict_mode(),
    has_constructor: |object| as_bound_function(object).bound_target_function().has_constructor(),
    get_stack_frame_info: BoundFunction::get_stack_frame_info,
    name_for_call_stack: |object| as_bound_function(object).name_for_call_stack(),
    ..FUNCTION_OBJECT_METHODS
};

define_cell!(BoundFunction, Object, extends: [FunctionObject, Object], methods: BOUND_FUNCTION_METHODS);

impl Deref for BoundFunction {
    type Target = FunctionObject;

    fn deref(&self) -> &FunctionObject {
        &self.base
    }
}

/// The bound function an internal method of a bound function was called on.
fn as_bound_function(object: &Object) -> &BoundFunction {
    assert!(object.is_bound_function());
    // SAFETY: The object is a BoundFunction, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<BoundFunction>() }
}

impl Object {
    pub fn is_bound_function(&self) -> bool {
        self.is::<BoundFunction>()
    }
}

impl BoundFunction {
    // 10.4.1.3 BoundFunctionCreate ( targetFunction, boundThis, boundArgs ), https://tc39.es/ecma262/#sec-boundfunctioncreate
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        target_function: Gc<FunctionObject>,
        bound_this: Value,
        bound_arguments: &[Value],
    ) -> ThrowCompletionOr<Gc<BoundFunction>> {
        // 1. Let proto be ? targetFunction.[[GetPrototypeOf]]().
        let prototype = target_function.internal_get_prototype_of(vm)?;

        // 2. Let internalSlotsList be the list-concatenation of « [[Prototype]], [[Extensible]] » and the internal slots listed in Table 34.
        // 3. Let obj be MakeBasicObject(internalSlotsList).
        // 4. Set obj.[[Prototype]] to proto.
        // 5. Set obj.[[Call]] as described in 10.4.1.1.
        // 6. If IsConstructor(targetFunction) is true, then
        //    a. Set obj.[[Construct]] as described in 10.4.1.2.
        // 7. Set obj.[[BoundTargetFunction]] to targetFunction.
        // 8. Set obj.[[BoundThis]] to boundThis.
        // 9. Set obj.[[BoundArguments]] to boundArgs.
        let object = allocate_object(
            vm,
            BoundFunction {
                base: FunctionObject::new_with_realm_and_prototype(
                    vm,
                    Self::CLASS,
                    realm,
                    prototype,
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                bound_target_function: Cell::new(target_function),
                bound_this: Cell::new(bound_this),
                bound_arguments: OnceCell::new(),
            },
        );
        // The caller keeps the bound arguments alive until the function that does holds them, and nothing allocates
        // from the heap in between.
        if object
            .bound_arguments
            .set(bound_arguments.to_vec().into_boxed_slice())
            .is_err()
        {
            unreachable!("the bound arguments are set once");
        }

        // 10. Return obj.
        Ok(object)
    }

    pub fn bound_target_function(&self) -> Gc<FunctionObject> {
        self.bound_target_function.get()
    }

    pub fn bound_this(&self) -> Value {
        self.bound_this.get()
    }

    pub fn bound_arguments_count(&self) -> usize {
        self.bound_arguments().len()
    }

    pub fn bound_argument(&self, index: usize) -> Value {
        self.bound_arguments()[index]
    }

    fn bound_arguments(&self) -> &[Value] {
        self.bound_arguments
            .get()
            .expect("a bound function has its bound arguments")
    }

    /// args is the list-concatenation of boundArgs and argumentsList, made in place in the callee's frame, whose
    /// argument slots have room for both.
    fn prepend_bound_arguments(&self, callee_context: &ExecutionContext) {
        let bound_arguments = self.bound_arguments();
        let argument_values = callee_context.arguments();
        let argument_count = argument_values.len();
        let mut index = argument_count;
        while index > bound_arguments.len() {
            index -= 1;
            argument_values[index].set(argument_values[index - bound_arguments.len()].get());
        }
        for (argument, bound_argument) in argument_values.iter().zip(bound_arguments) {
            argument.set(*bound_argument);
        }

        callee_context.passed_argument_count.set(
            callee_context.passed_argument_count.get()
                + u32::try_from(bound_arguments.len()).expect("the bound argument count fits in u32"),
        );
    }

    // 10.4.1.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-bound-function-exotic-objects-call-thisargument-argumentslist
    fn internal_call(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        _this_argument: Value,
    ) -> ThrowCompletionOr<Value> {
        let function = as_bound_function(object);

        // 1. Let target be F.[[BoundTargetFunction]].
        let target = function.bound_target_function();

        // 2. Let boundThis be F.[[BoundThis]].
        let bound_this = function.bound_this();

        // 3. Let boundArgs be F.[[BoundArguments]].
        // 4. Let args be the list-concatenation of boundArgs and argumentsList.
        function.prepend_bound_arguments(callee_context);

        // 5. Return ? Call(target, boundThis, args).
        let internal_call = target
            .internal_call_method()
            .expect("the target of a bound function is callable");
        internal_call(&target, vm, callee_context, bound_this)
    }

    // 10.4.1.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-bound-function-exotic-objects-construct-argumentslist-newtarget
    fn internal_construct(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        new_target: Gc<FunctionObject>,
    ) -> ThrowCompletionOr<Gc<Object>> {
        let function = as_bound_function(object);

        // 1. Let target be F.[[BoundTargetFunction]].
        let target = function.bound_target_function();

        // 2. Assert: IsConstructor(target) is true.
        assert!(Value::from_object(target).is_constructor());

        // 3. Let boundArgs be F.[[BoundArguments]].
        // 4. Let args be the list-concatenation of boundArgs and argumentsList.
        function.prepend_bound_arguments(callee_context);

        // 5. If SameValue(F, newTarget) is true, set newTarget to target.
        let mut final_new_target = new_target;
        if function.as_function_object_gc() == new_target {
            final_new_target = target;
        }

        // 6. Return ? Construct(target, args, newTarget).
        let internal_construct = target
            .internal_construct_method()
            .expect("the target of a bound constructor is a constructor");
        internal_construct(&target, vm, callee_context, final_new_target)
    }

    fn get_stack_frame_info(object: &Object, vm: &Vm, stack_frame_info: &mut StackFrameInfo) {
        let function = as_bound_function(object);
        function
            .bound_target_function()
            .get_stack_frame_info(vm, stack_frame_info);
        stack_frame_info.argument_count +=
            u32::try_from(function.bound_arguments_count()).expect("the bound argument count fits in u32");
    }

    pub fn name_for_call_stack(&self) -> Utf16String {
        self.bound_target_function().name_for_call_stack()
    }
}
