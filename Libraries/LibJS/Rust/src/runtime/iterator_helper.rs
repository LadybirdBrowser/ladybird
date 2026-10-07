/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Completion, ThrowCompletionOr};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::generator_object::{GeneratorObject, GeneratorState, IterationResult};
use crate::runtime::iterator::{IteratorRecord, iterator_close_all};
use crate::runtime::iterator_constructor::{ConcatIterator, ZipIterator};
use crate::runtime::iterator_prototype::{self, FlatMapIterator};
use crate::runtime::realm::Realm;

pub const ITERATOR_HELPER_BRAND: &str = "Iterator Helper";

/// The abstract closure an iterator helper runs, and the one it runs for an abrupt completion, with what they capture.
/// The closures are functions of the files whose built-ins create them, and the state that changes as the helper runs
/// lives in cells they capture.
#[derive(Clone, Copy, Trace)]
pub enum IteratorHelperClosure {
    Concat {
        iterables: Gc<ConcatIterator>,
    },
    Drop {
        iterated: Gc<IteratorRecord>,
        integer_limit: f64,
    },
    Filter {
        iterated: Gc<IteratorRecord>,
        predicate: Gc<FunctionObject>,
    },
    FlatMap {
        iterated: Gc<IteratorRecord>,
        flat_map_iterator: Gc<FlatMapIterator>,
        mapper: Gc<FunctionObject>,
    },
    Map {
        iterated: Gc<IteratorRecord>,
        mapper: Gc<FunctionObject>,
    },
    Take {
        iterated: Gc<IteratorRecord>,
        integer_limit: f64,
    },
    Zip {
        zip_iterator: Gc<ZipIterator>,
    },
}

impl IteratorHelperClosure {
    fn call(self, vm: &Vm, iterator: &IteratorHelper) -> ThrowCompletionOr<IterationResult> {
        match self {
            Self::Concat { iterables } => iterables.next(vm, iterator),
            Self::Drop {
                iterated,
                integer_limit,
            } => iterator_prototype::drop_closure(vm, iterator, iterated, integer_limit),
            Self::Filter { iterated, predicate } => {
                iterator_prototype::filter_closure(vm, iterator, iterated, predicate)
            }
            Self::FlatMap {
                iterated,
                flat_map_iterator,
                mapper,
            } => flat_map_iterator.next(vm, iterated, iterator, mapper),
            Self::Map { iterated, mapper } => iterator_prototype::map_closure(vm, iterator, iterated, mapper),
            Self::Take {
                iterated,
                integer_limit,
            } => iterator_prototype::take_closure(vm, iterator, iterated, integer_limit),
            Self::Zip { zip_iterator } => zip_iterator.next(vm),
        }
    }

    /// The abrupt closure, for the helpers that have one.
    fn abrupt_closure(self, vm: &Vm, completion: Completion) -> Option<ThrowCompletionOr<Value>> {
        match self {
            Self::Concat { iterables } => Some(iterables.on_abrupt_completion(vm, completion)),
            Self::FlatMap {
                iterated,
                flat_map_iterator,
                ..
            } => Some(flat_map_iterator.on_abrupt_completion(vm, iterated, completion)),
            Self::Zip { zip_iterator } => Some(zip_iterator.on_abrupt_completion(vm, completion)),
            Self::Drop { .. } | Self::Filter { .. } | Self::Map { .. } | Self::Take { .. } => None,
        }
    }
}

#[repr(C)]
#[derive(Trace)]
pub struct IteratorHelper {
    base: GeneratorObject,
    underlying_iterators: GcRefCell<Vec<Gc<IteratorRecord>>>, // [[UnderlyingIterators]]
    closure: Cell<IteratorHelperClosure>,
    #[gc(untraced)]
    counter: Cell<f64>,
}

define_cell!(IteratorHelper, Object, extends: [GeneratorObject, Object], finalize: finalize);

impl Deref for IteratorHelper {
    type Target = GeneratorObject;

    fn deref(&self) -> &GeneratorObject {
        &self.base
    }
}

impl Finalize for IteratorHelper {
    fn finalize(&self) {
        Finalize::finalize(&self.base);
        drop(self.underlying_iterators.replace(Vec::new()));
    }
}

impl IteratorHelper {
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        underlying_iterators: &MarkedVec<'_, Gc<IteratorRecord>>,
        closure: IteratorHelperClosure,
    ) -> Gc<IteratorHelper> {
        let prototype = realm.intrinsics().iterator_helper_prototype();
        let running_execution_context = vm
            .running_execution_context()
            .expect("an iterator helper is created by a running built-in");
        // SAFETY: The running execution context is live.
        let execution_context = unsafe { running_execution_context.as_ref() }.copy();
        let iterator = realm.create_object(
            vm,
            IteratorHelper {
                base: GeneratorObject::new(
                    vm,
                    Self::CLASS,
                    realm,
                    Some(prototype),
                    execution_context,
                    Some(ITERATOR_HELPER_BRAND),
                    IteratorHelper::execute,
                ),
                underlying_iterators: GcRefCell::new(Vec::new()),
                closure: Cell::new(closure),
                counter: Cell::new(0.0),
            },
        );
        *iterator.underlying_iterators.borrow_mut() = underlying_iterators.to_vec();
        iterator
    }

    /// A copy of [[UnderlyingIterators]], which stays alive while iterators are closed.
    pub fn underlying_iterators<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Gc<IteratorRecord>> {
        let underlying_iterators = MarkedVec::new(vm);
        for iterator_record in self.underlying_iterators.borrow().iter() {
            underlying_iterators.push(*iterator_record);
        }
        underlying_iterators
    }

    pub fn counter(&self) -> f64 {
        self.counter.get()
    }

    pub fn increment_counter(&self) {
        self.counter.set(self.counter.get() + 1.0);
    }

    fn execute(generator: &GeneratorObject, vm: &Vm, completion: Completion) -> ThrowCompletionOr<IterationResult> {
        assert!(generator.is::<IteratorHelper>());
        // SAFETY: Only iterator helpers execute through this, and an IteratorHelper starts with its GeneratorObject.
        let iterator = unsafe { &*core::ptr::from_ref(generator).cast::<IteratorHelper>() };
        let result = iterator.execute_closure(vm, completion);
        vm.pop_execution_context();
        result
    }

    fn execute_closure(&self, vm: &Vm, completion: Completion) -> ThrowCompletionOr<IterationResult> {
        let closure = self.closure.get();

        if completion.is_abrupt() {
            // NB: This leaves the helper executing when closing its iterators throws, so that it throws on every later
            //     call.
            let abrupt_result = match closure.abrupt_closure(vm, completion) {
                Some(abrupt_result) => abrupt_result?,
                None => {
                    iterator_close_all(vm, &self.underlying_iterators(vm), completion).into_throw_completion_or()?
                }
            };

            self.set_generator_state(GeneratorState::Completed);
            return Ok(IterationResult::new(abrupt_result, true));
        }

        let result_value = closure.call(vm, self);

        let result = match result_value {
            Err(throw) => {
                self.set_generator_state(GeneratorState::Completed);
                return Err(throw);
            }
            Ok(result) => result,
        };
        self.set_generator_state(if result.done {
            GeneratorState::Completed
        } else {
            GeneratorState::SuspendedYield
        });

        Ok(result)
    }
}
