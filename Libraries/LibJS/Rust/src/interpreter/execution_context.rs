/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
use core::ptr::NonNull;
use std::alloc::{Layout, alloc, dealloc, handle_alloc_error};

use crate::bytecode::executable::Executable;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::environment::{Environment, PrivateEnvironment};
use crate::layout::execution_context::{ExecutionContext, ScriptOrModule, ScriptOrModuleSlot};
use crate::layout::value::Value;
use crate::runtime::completion::Must;
use crate::runtime::ecmascript_function_object::{EcmascriptFunctionObject, as_ecmascript_function_object};
use crate::runtime::native_function::NativeFunction;
use crate::runtime::realm::Realm;
use crate::runtime::shared_function_instance_data::ThisMode;
use libjs_abi::register::RESERVED_REGISTER_COUNT;

impl ExecutionContext {
    /// The [[ScriptOrModule]] of the context. That of a frame with a function is the function's, which is null for a
    /// native function.
    pub fn script_or_module(&self) -> ScriptOrModule {
        match self.function.get() {
            Some(function) => as_ecmascript_function_object(function)
                .map_or(ScriptOrModule::Empty, |function| function.script_or_module()),
            None => self.script_or_module_without_function.get(),
        }
    }

    /// The [[Realm]] of the context. That of a frame with a function is the function's.
    pub fn realm(&self) -> Option<Gc<Realm>> {
        match self.function.get() {
            Some(function) => function.function_realm(),
            None => self.realm_without_function.get(),
        }
    }

    /// realm(), with the realm of a frame of an ECMAScript function read here: it is that of the function's shape (see
    /// ECMASCRIPT_FUNCTION_OBJECT_METHODS), which saves the call through the function's method table. For the paths
    /// that resume suspended frames, which ask on every resume.
    #[inline]
    pub fn realm_of_resumed_frame(&self) -> Option<Gc<Realm>> {
        if let Some(function) = self.function.get()
            && function.is_ecmascript_function_object()
        {
            return Some(function.shape().realm());
        }
        self.realm()
    }

    /// Sets the [[Realm]] of a context without a function. The context must not be running yet: the VM reads the
    /// realm of a context when it starts running (see `Vm::set_running_execution_context()`).
    pub fn set_realm(&self, realm: Option<Gc<Realm>>) {
        debug_assert!(
            self.function.get().is_none(),
            "a context with a function runs in the function's realm"
        );
        self.realm_without_function.set(realm);
    }

    /// Whether the context's code starts with `EnterWithOwnEnvironments`.
    fn runs_code_that_keeps_own_environments(&self) -> bool {
        self.executable
            .get()
            .is_some_and(|executable| Executable::from_head(executable).keeps_own_environments())
    }

    /// Whether the context keeps its own [[PrivateEnvironment]]: one without a function, or one whose code makes
    /// private environments.
    pub fn keeps_private_environment(&self) -> bool {
        self.function.get().is_none() || self.runs_code_that_keeps_own_environments()
    }

    /// Whether the context keeps its own [[VariableEnvironment]]: one without a function, one of a function that needs
    /// a function environment, or one whose code makes variable environments. That of another ECMAScript function is
    /// the function's [[Environment]], and a native function has none.
    pub fn keeps_variable_environment(&self) -> bool {
        let Some(function) = self.function.get() else {
            return true;
        };
        if let Some(function) = function.downcast::<EcmascriptFunctionObject>() {
            return function.function_environment_needed() || self.runs_code_that_keeps_own_environments();
        }
        function
            .downcast::<NativeFunction>()
            .is_some_and(|function| function.function_environment_needed())
    }

    /// The [[VariableEnvironment]] of the context.
    pub fn variable_environment(&self) -> Option<Gc<Environment>> {
        if self.keeps_variable_environment() {
            return self.variable_environment_in_frame.get();
        }
        self.function
            .get()
            .and_then(|function| function.downcast::<EcmascriptFunctionObject>())
            .and_then(|function| function.environment())
    }

    /// Sets the [[VariableEnvironment]] of a context that keeps its own.
    pub fn set_variable_environment(&self, variable_environment: Option<Gc<Environment>>) {
        debug_assert!(
            self.keeps_variable_environment(),
            "a context of a function without a function environment has its function's"
        );
        self.variable_environment_in_frame.set(variable_environment);
    }

    /// Whether the variable environment field holds the context's [[VariableEnvironment]], which the code of a
    /// context that keeps its own sets when it starts.
    fn holds_variable_environment(&self) -> bool {
        let Some(function) = self.function.get() else {
            return true;
        };
        match function.downcast::<EcmascriptFunctionObject>() {
            Some(function) => {
                function.function_environment_needed()
                    || (self.frame_initialized.get() && self.runs_code_that_keeps_own_environments())
            }
            None => self.keeps_variable_environment(),
        }
    }

    /// The [[PrivateEnvironment]] of the context. One that does not keep its own has its function's.
    pub fn private_environment(&self) -> Option<Gc<PrivateEnvironment>> {
        if self.keeps_private_environment() {
            return self.private_environment_in_frame.get();
        }
        self.function
            .get()
            .and_then(|function| function.downcast::<EcmascriptFunctionObject>())
            .and_then(|function| function.private_environment())
    }

    /// Sets the [[PrivateEnvironment]] of a context that keeps its own.
    pub fn set_private_environment(&self, private_environment: Option<Gc<PrivateEnvironment>>) {
        debug_assert!(
            self.keeps_private_environment(),
            "a context of code that makes no private environment has its function's"
        );
        self.private_environment_in_frame.set(private_environment);
    }

    /// Sets the [[ScriptOrModule]] of a context without a function.
    pub fn set_script_or_module(&self, script_or_module: ScriptOrModule) {
        debug_assert!(
            self.function.get().is_none(),
            "a context with a function has the function's ScriptOrModule"
        );
        self.script_or_module_without_function.set(script_or_module);
    }

    /// Constructs a context for a frame of `slot_count` slots, the last `argument_count` of which hold arguments, at
    /// `context`.
    ///
    /// # Safety
    ///
    /// `context` must point to writable memory for the context and its slots, which nothing else uses.
    pub(crate) unsafe fn initialize_at(
        context: *mut ExecutionContext,
        registers_and_locals_count: u32,
        slot_count: u32,
        argument_count: u32,
    ) {
        // SAFETY: The caller passes memory for the context and its slots.
        unsafe {
            context.write(ExecutionContext {
                function: Cell::new(None),
                realm_without_function: Cell::new(None),
                script_or_module_without_function: ScriptOrModuleSlot::new(ScriptOrModule::Empty),
                lexical_environment: Cell::new(None),
                variable_environment_in_frame: Cell::new(None),
                private_environment_in_frame: Cell::new(None),
                frame_id: Cell::new(0),
                program_counter: Cell::new(0),
                yield_continuation: Cell::new(ExecutionContext::NO_YIELD_CONTINUATION),
                yield_is_await: Cell::new(false),
                yield_value_is_iterator_result: Cell::new(false),
                caller_is_construct: Cell::new(false),
                frame_initialized: Cell::new(false),
                this_value: Cell::new(Value::EMPTY),
                executable: Cell::new(None),
                caller_frame: Cell::new(core::ptr::null_mut()),
                passed_argument_count: Cell::new(0),
                caller_return_pc: Cell::new(0),
                caller_dst_raw: Cell::new(0),
                registers_and_constants_and_locals_and_arguments_count: Cell::new(slot_count),
                argument_count: Cell::new(argument_count),
                returns_to_native_caller: Cell::new(false),
                runs_jit_code: Cell::new(false),
                caller_is_derived_construct: Cell::new(false),
                new_target: Cell::new(Value::UNDEFINED),
            });
            // NB: Enter initializes the remaining registers, locals, and constants.
            for slot in (*context)
                .slots()
                .iter()
                .take(registers_and_locals_count.min(RESERVED_REGISTER_COUNT) as usize)
            {
                slot.set(Value::EMPTY);
            }
        }
    }

    /// The frame's registers, locals, constants and arguments, which follow the context directly.
    pub fn slots(&self) -> &[Cell<Value>] {
        let count = self.registers_and_constants_and_locals_and_arguments_count.get() as usize;
        // SAFETY: Contexts are only ever allocated with their slots following them.
        unsafe { ak::slice_from_raw_parts(core::ptr::from_ref(self).add(1).cast::<Cell<Value>>(), count) }
    }

    pub fn register(&self, index: u32) -> &Cell<Value> {
        &self.slots()[index as usize]
    }

    pub fn arguments(&self) -> &[Cell<Value>] {
        let slots = self.slots();
        &slots[slots.len() - self.argument_count.get() as usize..]
    }

    pub fn argument(&self, index: usize) -> Value {
        self.arguments().get(index).map_or(Value::UNDEFINED, Cell::get)
    }

    /// ExecutionContext::copy(), for the generator that keeps the context of the call that created it.
    pub fn copy(&self) -> OwnedExecutionContext {
        let slot_count = self.registers_and_constants_and_locals_and_arguments_count.get();
        let argument_count = self.argument_count.get();
        // NB: We pass the entire non-argument count as registers_and_locals_count with 0 constants.
        let copy = OwnedExecutionContext::create(slot_count - argument_count, 0, argument_count);
        copy.function.set(self.function.get());
        copy.realm_without_function.set(self.realm_without_function.get());
        copy.script_or_module_without_function
            .set(self.script_or_module_without_function.get());
        copy.lexical_environment.set(self.lexical_environment.get());
        copy.variable_environment_in_frame
            .set(self.variable_environment_in_frame.get());
        copy.private_environment_in_frame
            .set(self.private_environment_in_frame.get());
        copy.program_counter.set(self.program_counter.get());
        copy.frame_id.set(self.frame_id.get());
        copy.yield_continuation.set(self.yield_continuation.get());
        copy.yield_is_await.set(self.yield_is_await.get());
        copy.yield_value_is_iterator_result
            .set(self.yield_value_is_iterator_result.get());
        copy.caller_is_construct.set(self.caller_is_construct.get());
        copy.caller_is_derived_construct
            .set(self.caller_is_derived_construct.get());
        copy.frame_initialized.set(self.frame_initialized.get());
        copy.this_value.set(self.this_value.get());
        copy.new_target.set(self.new_target.get());
        copy.executable.set(self.executable.get());
        copy.passed_argument_count.set(self.passed_argument_count.get());
        let frame_initialized = self.frame_initialized.get();
        let non_argument_count = (slot_count - argument_count) as usize;
        for (index, (copied_slot, slot)) in copy.slots().iter().zip(self.slots()).enumerate() {
            if !frame_initialized && index >= RESERVED_REGISTER_COUNT as usize && index < non_argument_count {
                continue;
            }
            copied_slot.set(slot.get());
        }
        copy
    }

    /// A copy of the context that runs nothing: an uninitialized frame of the same function, realm, script and
    /// environments, whose slots hold no values. It keeps none of the values the context's frame held alive.
    pub fn copy_without_values(&self) -> OwnedExecutionContext {
        let slot_count = self.registers_and_constants_and_locals_and_arguments_count.get();
        let argument_count = self.argument_count.get();
        let copy = OwnedExecutionContext::create(slot_count - argument_count, 0, argument_count);
        copy.function.set(self.function.get());
        copy.realm_without_function.set(self.realm_without_function.get());
        copy.script_or_module_without_function
            .set(self.script_or_module_without_function.get());
        copy.lexical_environment.set(self.lexical_environment.get());
        copy.variable_environment_in_frame
            .set(self.variable_environment_in_frame.get());
        copy.private_environment_in_frame
            .set(self.private_environment_in_frame.get());
        copy.program_counter.set(self.program_counter.get());
        copy.frame_id.set(self.frame_id.get());
        copy.caller_is_construct.set(self.caller_is_construct.get());
        copy.caller_is_derived_construct
            .set(self.caller_is_derived_construct.get());
        copy.this_value.set(self.this_value.get());
        copy.new_target.set(self.new_target.get());
        copy.executable.set(self.executable.get());
        // NB: An uninitialized frame holds values only in its reserved registers, which the new context starts with
        //     empty, and its arguments.
        for argument in copy.arguments() {
            argument.set(Value::EMPTY);
        }
        copy
    }
}

// SAFETY: Visits every cell a frame can reach. Until the frame is initialized only its reserved registers and its
// arguments hold values; the other slots are left over from earlier frames.
unsafe impl Trace for ExecutionContext {
    fn trace(&self, visitor: &mut Visitor) {
        self.function.trace(visitor);
        if self.holds_variable_environment() {
            self.variable_environment_in_frame.trace(visitor);
        }
        self.lexical_environment.trace(visitor);
        self.this_value.trace(visitor);
        self.new_target.trace(visitor);
        self.executable.trace(visitor);
        // NB: The field of a frame with a function holds an old value, as script_or_module() says.
        // NB: Until it is initialized, a frame of code that makes private environments holds an old value.
        if self.function.get().is_none() || (self.frame_initialized.get() && self.keeps_private_environment()) {
            self.private_environment_in_frame.trace(visitor);
        }
        // NB: A frame with a function holds an old value there too, apart from the lightweight frame of a native
        //     function call, which has no executable and keeps the caller's realm.
        if self.function.get().is_none() || self.executable.get().is_none() {
            self.realm_without_function.trace(visitor);
        }
        if self.function.get().is_none() {
            match self.script_or_module_without_function.get() {
                ScriptOrModule::Empty => {}
                ScriptOrModule::Script(script) => script.trace(visitor),
                ScriptOrModule::Module(module) => module.trace(visitor),
            }
        }
        let slots = self.slots();
        if self.frame_initialized.get() {
            trace_slots(slots, visitor);
        } else {
            let non_argument_count = slots.len() - self.argument_count.get() as usize;
            trace_slots(
                &slots[..non_argument_count.min(RESERVED_REGISTER_COUNT as usize)],
                visitor,
            );
            trace_slots(self.arguments(), visitor);
        }
    }
}

fn trace_slots(slots: &[Cell<Value>], visitor: &mut Visitor) {
    // SAFETY: Cell<Value> has the same layout as Value, and nothing writes the slots while they are visited.
    let values = unsafe { ak::slice_from_raw_parts(slots.as_ptr().cast::<Value>(), slots.len()) };
    visitor.visit_values(values);
}

/// The slot counts that the slots of a context outside the interpreter stack are rounded up to. Contexts of these sizes
/// are not freed, but kept for the next context of their size, as the host creates and drops one for many callbacks
/// and promise jobs.
const REUSED_SLOT_COUNTS: [u32; 6] = [4, 16, 64, 128, 256, 512];

/// The contexts of each of REUSED_SLOT_COUNTS that were dropped on this thread, which is the thread of its VM.
struct ReusableExecutionContexts([Vec<NonNull<ExecutionContext>>; REUSED_SLOT_COUNTS.len()]);

impl Drop for ReusableExecutionContexts {
    fn drop(&mut self) {
        for (contexts, &slot_count) in self.0.iter().zip(REUSED_SLOT_COUNTS.iter()) {
            for context in contexts {
                // SAFETY: The context was allocated with the layout of its reused slot count, and nothing uses it.
                unsafe {
                    dealloc(
                        context.as_ptr().cast(),
                        OwnedExecutionContext::layout_for_slot_count(slot_count),
                    );
                }
            }
        }
    }
}

thread_local! {
    static REUSABLE_EXECUTION_CONTEXTS: RefCell<ReusableExecutionContexts> =
        const { RefCell::new(ReusableExecutionContexts([const { Vec::new() }; REUSED_SLOT_COUNTS.len()])) };
}

/// The index in REUSED_SLOT_COUNTS of the size a context of `slot_count` slots is allocated with, if it is reused.
fn reused_slot_count_index(slot_count: u32) -> Option<usize> {
    REUSED_SLOT_COUNTS
        .iter()
        .position(|&reused_slot_count| slot_count <= reused_slot_count)
}

/// An execution context that lives outside the interpreter stack, such as those made for realms. It must be popped off
/// the execution context stack before it is dropped.
pub struct OwnedExecutionContext {
    context: NonNull<ExecutionContext>,
}

impl OwnedExecutionContext {
    /// ExecutionContext::create(registers_and_locals_count, constants, arguments_count)
    pub fn create(registers_and_locals_count: u32, constant_count: u32, argument_count: u32) -> Self {
        let slot_count = registers_and_locals_count
            .checked_add(constant_count)
            .and_then(|count| count.checked_add(argument_count))
            .expect("the slot count of an execution context fits in u32");
        let reused_slot_count_index = reused_slot_count_index(slot_count);
        // The free lists are gone once the thread's thread-locals are destroyed, which on macOS happens before C++
        // static destructors run, so a context made or dropped after that is allocated or freed directly.
        let reused_context = reused_slot_count_index.and_then(|index| {
            REUSABLE_EXECUTION_CONTEXTS
                .try_with(|reusable_contexts| reusable_contexts.borrow_mut().0[index].pop())
                .ok()
                .flatten()
        });
        let context = reused_context.unwrap_or_else(|| {
            let allocated_slot_count = reused_slot_count_index.map_or(slot_count, |index| REUSED_SLOT_COUNTS[index]);
            let layout = Self::layout_for_slot_count(allocated_slot_count);
            // SAFETY: The layout has room for at least the context.
            let memory = unsafe { alloc(layout) }.cast::<ExecutionContext>();
            NonNull::new(memory).unwrap_or_else(|| handle_alloc_error(layout))
        });
        // SAFETY: The memory has room for the context and its slots, and nothing else uses it.
        unsafe {
            ExecutionContext::initialize_at(context.as_ptr(), registers_and_locals_count, slot_count, argument_count);
        }
        Self { context }
    }

    fn layout_for_slot_count(slot_count: u32) -> Layout {
        Layout::from_size_align(
            size_of::<ExecutionContext>() + slot_count as usize * size_of::<Value>(),
            align_of::<ExecutionContext>(),
        )
        .expect("the layout of an execution context is valid")
    }

    pub fn as_non_null(&self) -> NonNull<ExecutionContext> {
        self.context
    }

    /// Gives up ownership of the context, which from_raw() takes back.
    pub fn into_raw(self) -> NonNull<ExecutionContext> {
        let context = self.context;
        core::mem::forget(self);
        context
    }

    /// Takes back ownership of a context that into_raw() gave up.
    ///
    /// # Safety
    ///
    /// `context` must come from into_raw(), and nothing may own it already.
    pub unsafe fn from_raw(context: NonNull<ExecutionContext>) -> Self {
        Self { context }
    }
}

// SAFETY: Visits every cell the context reaches.
unsafe impl Trace for OwnedExecutionContext {
    fn trace(&self, visitor: &mut Visitor) {
        (**self).trace(visitor);
    }
}

impl core::ops::Deref for OwnedExecutionContext {
    type Target = ExecutionContext;

    fn deref(&self) -> &ExecutionContext {
        // SAFETY: The context lives until this is dropped.
        unsafe { self.context.as_ref() }
    }
}

impl Drop for OwnedExecutionContext {
    fn drop(&mut self) {
        // This finds the size of the allocation from the context's slot count, which never changes after it is created.
        let slot_count = self.registers_and_constants_and_locals_and_arguments_count.get();
        let allocated_slot_count = match reused_slot_count_index(slot_count) {
            Some(index) => {
                let context = self.context;
                let returned_to_free_list = REUSABLE_EXECUTION_CONTEXTS
                    .try_with(|reusable_contexts| reusable_contexts.borrow_mut().0[index].push(context))
                    .is_ok();
                if returned_to_free_list {
                    return;
                }
                REUSED_SLOT_COUNTS[index]
            }
            None => slot_count,
        };
        // SAFETY: The context was allocated with this layout, and contexts hold nothing that needs dropping.
        unsafe {
            dealloc(
                self.context.as_ptr().cast(),
                Self::layout_for_slot_count(allocated_slot_count),
            );
        }
    }
}

/// Binds the this value of `frame` like OrdinaryCallBindThis(), if it runs a sloppy function that uses it and has an
/// unbound one: callers pass the this value as it is, and the function starts with `EnterCoercingThis` (or
/// `EnterWithOwnEnvironments`). Frames that the runtime made have a bound one already.
pub fn coerce_this(vm: &Vm, frame: &ExecutionContext) {
    let register = frame.register(libjs_abi::register::THIS_VALUE);
    let this_argument = register.get();
    if this_argument.is_object() || this_argument.is_empty() {
        return;
    }
    let Some(function) = frame
        .function
        .get()
        .and_then(|function| function.downcast::<EcmascriptFunctionObject>())
    else {
        return;
    };
    let shared_data = function.shared_data();
    if !shared_data.uses_this() || function.this_mode() != ThisMode::Global {
        return;
    }
    // NB: A call into another realm goes through the runtime, so the function runs in the current realm.
    let this_value = if this_argument.is_nullish() {
        let realm = vm.current_realm().expect("a function runs in a realm");
        Value::from_object(realm.global_environment().global_this_value())
    } else {
        Value::from_object(this_argument.to_object(vm).must())
    };
    register.set(this_value);
    frame.this_value.set(this_value);
}
