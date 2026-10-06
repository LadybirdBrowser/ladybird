/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;
use core::ptr::NonNull;

use libjs_runtime_macros::Trace;

use crate::bytecode::executable::Executable;
use crate::gc::class::{Class, Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::execution_context::OwnedExecutionContext;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::get_prototype_from_constructor;
use crate::runtime::completion::{Completion, CompletionType, Must, Throw, ThrowCompletionOr};
use crate::runtime::ecmascript_function_object::EcmascriptFunctionObject;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::shared_function_instance_data::FunctionKind;

/// Variant<GC::Ref<ECMAScriptFunctionObject>, GC::Ref<NativeJavaScriptBackedFunction>>: the function whose body a
/// generator or an async generator runs.
#[derive(Clone, Copy)]
pub enum GeneratingFunction {
    Ecmascript(Gc<EcmascriptFunctionObject>),
    NativeJavaScriptBacked(Gc<NativeJavaScriptBackedFunction>),
}

impl GeneratingFunction {
    pub fn kind(self) -> FunctionKind {
        match self {
            GeneratingFunction::Ecmascript(function) => function.kind(),
            GeneratingFunction::NativeJavaScriptBacked(function) => function.kind(),
        }
    }

    pub fn as_function_object(self) -> Gc<FunctionObject> {
        match self {
            GeneratingFunction::Ecmascript(function) => function.upcast(),
            GeneratingFunction::NativeJavaScriptBacked(function) => function.upcast(),
        }
    }

    pub fn bytecode_executable(self, vm: &Vm) -> Gc<Executable> {
        match self {
            GeneratingFunction::Ecmascript(function) => function
                .bytecode_executable()
                .expect("a generating function is compiled before it is called"),
            GeneratingFunction::NativeJavaScriptBacked(function) => function.bytecode_executable(vm),
        }
    }
}

/// GeneratorObject::IterationResult: what resuming a generator produced.
#[derive(Clone, Copy, Debug)]
pub struct IterationResult {
    pub done: bool,
    pub value: Value,
    /// Whether `value` already is the iterator result object to hand out, as yield* forwards the results of the
    /// iterator it delegates to.
    pub value_is_iterator_result: bool,
}

impl IterationResult {
    pub fn new(value: Value, done: bool) -> Self {
        Self::new_with_value_is_iterator_result(value, done, false)
    }

    pub fn new_with_value_is_iterator_result(value: Value, done: bool, value_is_iterator_result: bool) -> Self {
        Self {
            done,
            value,
            value_is_iterator_result,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Trace)]
pub enum GeneratorState {
    SuspendedStart,
    SuspendedYield,
    Executing,
    Completed,
}

/// The virtual GeneratorObject::execute(), which IteratorHelper overrides.
pub type GeneratorExecuteFunction = fn(&GeneratorObject, &Vm, Completion) -> ThrowCompletionOr<IterationResult>;

#[repr(C)]
#[derive(Trace)]
pub struct GeneratorObject {
    base: Object,
    execution_context: GcRefCell<Option<OwnedExecutionContext>>,
    generating_executable: Cell<Option<Gc<Executable>>>,
    yield_continuation: Cell<u32>,
    generator_state: Cell<GeneratorState>,
    generator_brand: Option<&'static str>,
    pending_completion_value: Cell<Value>,
    #[gc(untraced)]
    pending_completion_type: Cell<CompletionType>,
    #[gc(untraced)]
    execute: GeneratorExecuteFunction,
}

define_cell!(GeneratorObject, Object, extends: [Object], finalize: finalize);

impl Deref for GeneratorObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for GeneratorObject {
    fn finalize(&self) {
        drop(self.execution_context.replace(None));
    }
}

impl GeneratorObject {
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        generating_function: GeneratingFunction,
        execution_context: OwnedExecutionContext,
    ) -> Gc<GeneratorObject> {
        let kind = generating_function.kind();

        let generating_function_prototype_object = if kind == FunctionKind::Async {
            // We implement async functions by transforming them to generator function in the bytecode
            // interpreter. However an async function does not have a prototype and should not be
            // changed thus we hardcode the prototype.
            realm.intrinsics().generator_prototype()
        } else {
            // 1. Let _generator_ be ? OrdinaryCreateFromConstructor(_functionObject_, *"%GeneratorPrototype%"*,
            //    « [[GeneratorState]], [[GeneratorContext]], [[GeneratorBrand]] »).
            get_prototype_from_constructor(vm, generating_function.as_function_object(), |intrinsics, _| {
                intrinsics.generator_prototype()
            })
            .must()
        };

        let generating_executable = generating_function.bytecode_executable(vm);

        let yield_continuation = execution_context.yield_continuation.get();
        let object = realm.create_object(
            vm,
            GeneratorObject::new(
                vm,
                Self::CLASS,
                realm,
                Some(generating_function_prototype_object),
                execution_context,
                None,
                GeneratorObject::execute,
            ),
        );
        object.generating_executable.set(Some(generating_executable));
        object.yield_continuation.set(yield_continuation);
        object
    }

    /// The constructor of GeneratorObject, for it and the classes that extend it.
    pub fn new(
        vm: &Vm,
        class: &'static Class,
        realm: Gc<Realm>,
        prototype: Option<Gc<Object>>,
        execution_context: OwnedExecutionContext,
        generator_brand: Option<&'static str>,
        execute: GeneratorExecuteFunction,
    ) -> GeneratorObject {
        GeneratorObject {
            base: Object::new_with_realm_and_prototype(
                vm,
                class,
                realm,
                prototype,
                MayInterfereWithIndexedPropertyAccess::No,
            ),
            execution_context: GcRefCell::new(Some(execution_context)),
            generating_executable: Cell::new(None),
            yield_continuation: Cell::new(ExecutionContext::NO_YIELD_CONTINUATION),
            generator_state: Cell::new(GeneratorState::SuspendedStart),
            generator_brand,
            pending_completion_value: Cell::new(Value::UNDEFINED),
            pending_completion_type: Cell::new(CompletionType::Normal),
            execute,
        }
    }

    pub fn as_gc(&self) -> Gc<GeneratorObject> {
        // SAFETY: Generator objects only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }

    /// [[GeneratorContext]]. It lives out of line until the generator is finalized, so the pointer stays valid for
    /// as long as the generator does.
    fn generator_context(&self) -> NonNull<ExecutionContext> {
        self.execution_context
            .borrow()
            .as_ref()
            .expect("a live generator has its execution context")
            .as_non_null()
    }

    pub fn generator_state(&self) -> GeneratorState {
        self.generator_state.get()
    }

    pub fn set_generator_state(&self, generator_state: GeneratorState) {
        self.generator_state.set(generator_state);
    }

    pub fn set_pending_completion(&self, completion: Completion) {
        self.pending_completion_value.set(completion.value());
        self.pending_completion_type.set(completion.completion_type());
    }

    pub fn pending_completion_value(&self) -> Value {
        self.pending_completion_value.get()
    }

    pub fn pending_completion_type(&self) -> CompletionType {
        self.pending_completion_type.get()
    }

    pub fn set_pending_completion_type(&self, completion_type: CompletionType) {
        self.pending_completion_type.set(completion_type);
    }

    pub fn clear_pending_completion(&self) {
        self.pending_completion_value.set(Value::UNDEFINED);
        self.pending_completion_type.set(CompletionType::Normal);
    }

    // 27.5.3.2 GeneratorValidate ( generator, generatorBrand ), https://tc39.es/ecma262/#sec-generatorvalidate
    fn validate(&self, vm: &Vm, generator_brand: Option<&str>) -> ThrowCompletionOr<GeneratorState> {
        // 1. Perform ? RequireInternalSlot(generator, [[GeneratorState]]).
        // 2. Perform ? RequireInternalSlot(generator, [[GeneratorBrand]]).
        // NOTE: Already done by the caller of resume or resume_abrupt, as they wouldn't have a GeneratorObject otherwise.

        // 3. If generator.[[GeneratorBrand]] is not the same value as generatorBrand, throw a TypeError exception.
        if self.generator_brand != generator_brand {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::GeneratorBrandMismatch,
                &[
                    &self.generator_brand.unwrap_or("<empty>"),
                    &generator_brand.unwrap_or("<empty>"),
                ],
            );
        }

        // 4. Assert: generator also has a [[GeneratorContext]] internal slot.
        // NOTE: Done by already being a GeneratorObject.

        // 5. Let state be generator.[[GeneratorState]].
        let state = self.generator_state.get();

        // 6. If state is executing, throw a TypeError exception.
        if state == GeneratorState::Executing {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::GeneratorAlreadyExecuting, &[]);
        }

        // 7. Return state.
        Ok(state)
    }

    fn execute(&self, vm: &Vm, completion: Completion) -> ThrowCompletionOr<IterationResult> {
        // Loosely based on step 4 of https://tc39.es/ecma262/#sec-generatorstart mixed with https://tc39.es/ecma262/#sec-generatoryield at the end.

        self.set_pending_completion(completion);

        // We should never enter `execute` again after the generator is complete.
        let yield_continuation = self.yield_continuation.get();
        assert!(yield_continuation != ExecutionContext::NO_YIELD_CONTINUATION);

        // Clear yield state so that a normal return (no yield) is detected as done.
        {
            // SAFETY: Only finalizing the generator frees its context, and the generator is alive while it runs.
            let execution_context = unsafe { self.generator_context().as_ref() };
            execution_context
                .yield_continuation
                .set(ExecutionContext::NO_YIELD_CONTINUATION);
            execution_context.yield_value_is_iterator_result.set(false);
        }

        let result_value = vm.run_executable_with_initial_accumulator_value(
            vm.running_execution_context()
                .expect("the generator's context is running"),
            self.generating_executable
                .get()
                .expect("a generator created by a function has its executable"),
            yield_continuation,
            Value::from_object(self.as_gc()),
        );
        self.clear_pending_completion();

        vm.pop_execution_context();

        let value = match result_value {
            // Uncaught exceptions disable the generator.
            Err(exception) => {
                self.generator_state.set(GeneratorState::Completed);
                return Err(Throw::new(exception));
            }
            Ok(value) => value,
        };

        let value = if value.is_empty() { Value::UNDEFINED } else { value };

        // SAFETY: As above.
        let execution_context = unsafe { self.generator_context().as_ref() };
        self.yield_continuation.set(execution_context.yield_continuation.get());
        let done = self.yield_continuation.get() == ExecutionContext::NO_YIELD_CONTINUATION;
        let value_is_iterator_result = execution_context.yield_value_is_iterator_result.get();

        self.generator_state.set(if done {
            GeneratorState::Completed
        } else {
            GeneratorState::SuspendedYield
        });

        Ok(IterationResult::new_with_value_is_iterator_result(
            value,
            done,
            value_is_iterator_result && !done,
        ))
    }

    // 27.5.3.3 GeneratorResume ( generator, value, generatorBrand ), https://tc39.es/ecma262/#sec-generatorresume
    pub fn resume(&self, vm: &Vm, value: Value, generator_brand: Option<&str>) -> ThrowCompletionOr<IterationResult> {
        // 1. Let state be ? GeneratorValidate(generator, generatorBrand).
        let state = self.validate(vm, generator_brand)?;

        // 2. If state is completed, return CreateIterResultObject(undefined, true).
        if state == GeneratorState::Completed {
            return Ok(IterationResult::new(Value::UNDEFINED, true));
        }

        // 3. Assert: state is either suspendedStart or suspendedYield.
        assert!(state == GeneratorState::SuspendedStart || state == GeneratorState::SuspendedYield);

        // 4. Let genContext be generator.[[GeneratorContext]].
        let generator_context = self.generator_context();

        // 5. Let methodContext be the running execution context.
        let method_context = vm.running_execution_context();

        // 6. Suspend methodContext.
        // 8. Push genContext onto the execution context stack; genContext is now the running execution context.
        // NOTE: This is done out of order as to not permanently disable the generator if push_execution_context throws,
        //       as `resume` will immediately throw when [[GeneratorState]] is "executing", never allowing the state to change.
        vm.push_execution_context_checking_stack_space(generator_context)?;

        // 7. Set generator.[[GeneratorState]] to executing.
        self.generator_state.set(GeneratorState::Executing);

        // 9. Resume the suspended evaluation of genContext using NormalCompletion(value) as the result of the operation that suspended it. Let result be the value returned by the resumed computation.
        let result = (self.execute)(self, vm, Completion::normal(value));

        // 10. Assert: When we return here, genContext has already been removed from the execution context stack and methodContext is the currently running execution context.
        assert!(vm.running_execution_context() == method_context);

        // 11. Return ? result.
        result
    }

    // 27.5.3.4 GeneratorResumeAbrupt ( generator, abruptCompletion, generatorBrand ), https://tc39.es/ecma262/#sec-generatorresumeabrupt
    pub fn resume_abrupt(
        &self,
        vm: &Vm,
        abrupt_completion: Completion,
        generator_brand: Option<&str>,
    ) -> ThrowCompletionOr<IterationResult> {
        // 1. Let state be ? GeneratorValidate(generator, generatorBrand).
        let mut state = self.validate(vm, generator_brand)?;

        // 2. If state is suspendedStart, then
        if state == GeneratorState::SuspendedStart {
            // a. Set generator.[[GeneratorState]] to completed.
            self.generator_state.set(GeneratorState::Completed);

            // b. Once a generator enters the completed state it never leaves it and its associated execution context is never resumed. Any execution state associated with generator can be discarded at this point.
            // We don't currently discard anything.

            // c. Set state to completed.
            state = GeneratorState::Completed;
        }

        // 3. If state is completed, then
        if state == GeneratorState::Completed {
            // a. If abruptCompletion.[[Type]] is return, then
            if abrupt_completion.completion_type() == CompletionType::Return {
                // i. Return CreateIterResultObject(abruptCompletion.[[Value]], true).
                return Ok(IterationResult::new(abrupt_completion.value(), true));
            }

            // b. Return ? abruptCompletion.
            return Err(abrupt_completion.release_error());
        }

        // 4. Assert: state is suspendedYield.
        assert!(state == GeneratorState::SuspendedYield);

        // 5. Let genContext be generator.[[GeneratorContext]].
        let generator_context = self.generator_context();

        // 6. Let methodContext be the running execution context.
        let method_context = vm.running_execution_context();

        // 7. Suspend methodContext.
        // 9. Push genContext onto the execution context stack; genContext is now the running execution context.
        // NOTE: This is done out of order as to not permanently disable the generator if push_execution_context throws,
        //       as `resume_abrupt` will immediately throw when [[GeneratorState]] is "executing", never allowing the state to change.
        vm.push_execution_context_checking_stack_space(generator_context)?;

        // 8. Set generator.[[GeneratorState]] to executing.
        self.generator_state.set(GeneratorState::Executing);

        // 10. Resume the suspended evaluation of genContext using abruptCompletion as the result of the operation that suspended it. Let result be the Completion Record returned by the resumed computation.
        let result = (self.execute)(self, vm, abrupt_completion);

        // 11. Assert: When we return here, genContext has already been removed from the execution context stack and methodContext is the currently running execution context.
        assert!(vm.running_execution_context() == method_context);

        // 12. Return ? result.
        result
    }
}
