/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GC::Function as the C++ implementation used it: steps that run later, which keep their captures alive until then.

use core::any::Any;
use core::marker::PhantomData;

use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Value, Visitor, Vm};

use crate::bindings::WebIdlResult;

trait Steps: Trace {
    /// Takes the argument, an Option<A>, and runs the steps with it.
    fn call(&self, vm: &Vm, argument: &mut dyn Any);
}

struct CapturedSteps<C, A> {
    captures: C,
    steps: fn(&Vm, &C, A),
}

// SAFETY: The steps are a plain function, which holds no cells.
unsafe impl<C: Trace, A> Trace for CapturedSteps<C, A> {
    fn trace(&self, visitor: &mut Visitor) {
        self.captures.trace(visitor);
    }
}

impl<C: Trace, A: 'static> Steps for CapturedSteps<C, A> {
    fn call(&self, vm: &Vm, argument: &mut dyn Any) {
        let argument = argument
            .downcast_mut::<Option<A>>()
            .and_then(Option::take)
            .expect("a function is called with an argument of its type");
        (self.steps)(vm, &self.captures, argument);
    }
}

pub(crate) struct FunctionCell(Box<dyn Steps>);

// SAFETY: The captures of the steps are all the cells this reaches.
unsafe impl Trace for FunctionCell {
    fn trace(&self, visitor: &mut Visitor) {
        self.0.trace(visitor);
    }
}

static FUNCTION_CLASS: EmbedderClass<FunctionCell> = EmbedderClass::new("IndexedDBFunction");

impl EmbedderValue for FunctionCell {
    const CLASS: &'static EmbedderClass<Self> = &FUNCTION_CLASS;
}

/// A GC::Function<void(A)>.
pub(crate) struct FunctionWith<A> {
    cell: Gc<EmbedderCell<FunctionCell>>,
    _argument: PhantomData<fn(A)>,
}

impl<A> Clone for FunctionWith<A> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<A> Copy for FunctionWith<A> {}

// SAFETY: The cell is all the function reaches.
unsafe impl<A> Trace for FunctionWith<A> {
    fn trace(&self, visitor: &mut Visitor) {
        self.cell.trace(visitor);
    }
}

/// A GC::Function<void()>.
pub(crate) type Function = FunctionWith<()>;

/// GC::create_function(), with captures that the function keeps alive.
pub(crate) fn create_function_with<A: 'static, C: Trace + 'static>(
    vm: &Vm,
    captures: C,
    steps: fn(&Vm, &C, A),
) -> FunctionWith<A> {
    let cell = FUNCTION_CLASS.allocate(vm, |_| {
        let steps: Box<dyn Steps> = Box::new(CapturedSteps { captures, steps });
        FunctionCell(steps)
    });
    FunctionWith {
        cell,
        _argument: PhantomData,
    }
}

pub(crate) fn create_function<C: Trace + 'static>(vm: &Vm, captures: C, steps: fn(&Vm, &C, ())) -> Function {
    create_function_with(vm, captures, steps)
}

impl<A: 'static> FunctionWith<A> {
    pub(crate) fn call(self, vm: &Vm, argument: A) {
        let mut argument = Some(argument);
        self.cell.0.call(vm, &mut argument);
        // NB: The function keeps its captures alive while it runs.
        core::hint::black_box(self.cell);
    }
}

pub(crate) fn call_function(vm: &Vm, function: Function) {
    function.call(vm, ());
}

/// A value that holds no cells, for the captures of a function: keys and plain data.
#[derive(Clone)]
pub(crate) struct NoCells<T>(pub(crate) T);

// SAFETY: The value holds no cells.
unsafe impl<T> Trace for NoCells<T> {
    fn trace(&self, _: &mut Visitor) {}
}

trait OperationSteps: Trace {
    fn call(&self, vm: &Vm) -> WebIdlResult<Value>;
}

struct CapturedOperation<C> {
    captures: C,
    steps: fn(&Vm, &C) -> WebIdlResult<Value>,
}

// SAFETY: The steps are a plain function, which holds no cells.
unsafe impl<C: Trace> Trace for CapturedOperation<C> {
    fn trace(&self, visitor: &mut Visitor) {
        self.captures.trace(visitor);
    }
}

impl<C: Trace> OperationSteps for CapturedOperation<C> {
    fn call(&self, vm: &Vm) -> WebIdlResult<Value> {
        (self.steps)(vm, &self.captures)
    }
}

pub(crate) struct OperationCell(Box<dyn OperationSteps>);

// SAFETY: The captures of the steps are all the cells this reaches.
unsafe impl Trace for OperationCell {
    fn trace(&self, visitor: &mut Visitor) {
        self.0.trace(visitor);
    }
}

static OPERATION_CLASS: EmbedderClass<OperationCell> = EmbedderClass::new("IndexedDBOperation");

impl EmbedderValue for OperationCell {
    const CLASS: &'static EmbedderClass<Self> = &OPERATION_CLASS;
}

/// A GC::Function<WebIDL::ExceptionOr<JS::Value>()>: the operation of a request.
pub(crate) type Operation = Gc<EmbedderCell<OperationCell>>;

/// GC::create_function() for an operation, with captures that it keeps alive.
pub(crate) fn create_operation<C: Trace + 'static>(
    vm: &Vm,
    captures: C,
    steps: fn(&Vm, &C) -> WebIdlResult<Value>,
) -> Operation {
    OPERATION_CLASS.allocate(vm, |_| {
        let steps: Box<dyn OperationSteps> = Box::new(CapturedOperation { captures, steps });
        OperationCell(steps)
    })
}

pub(crate) fn perform_operation(vm: &Vm, operation: Operation) -> WebIdlResult<Value> {
    let result = operation.0.call(vm);
    // NB: The operation keeps its captures alive while it runs.
    core::hint::black_box(operation);
    result
}

impl Function {
    /// The cell, for LibWeb's C++ code to hold until it runs the function.
    pub(crate) fn cell_as_ptr(self) -> *mut core::ffi::c_void {
        self.cell.as_ptr().cast()
    }

    /// The function whose cell cell_as_ptr() handed out.
    ///
    /// # Safety
    ///
    /// The cell must be that of a function that takes no argument.
    pub(crate) unsafe fn from_cell(cell: Gc<EmbedderCell<FunctionCell>>) -> Self {
        Self {
            cell,
            _argument: PhantomData,
        }
    }
}
