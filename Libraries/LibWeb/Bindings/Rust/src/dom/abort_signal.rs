/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, MarkedVec, NativeFunction, Realm, Throw, Trace, Value, Vm, call};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::global_object::window_or_worker_global_scope_from_global_object;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi, cell_into_abi, realm_arguments};
use crate::bindings::promise::{TaskSource, exception_value, queue_a_global_task};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_dom_run_abort_algorithm(algorithm: *mut c_void);
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AbortAlgorithm {
    Cpp(CppCell),
    Rust(Gc<NativeFunction>),
}

impl AbortAlgorithm {
    fn run(&self, vm: &Vm) {
        match self {
            Self::Cpp(algorithm) => {
                // SAFETY: The algorithm is a live GC::Function<void()>.
                unsafe { web_dom_run_abort_algorithm(algorithm.as_ptr()) };
            }
            Self::Rust(algorithm) => {
                // NB: The algorithm cannot fail, but it can run into the abort of the script that it calls.
                let result = call(vm, Value::from_object(*algorithm), Value::UNDEFINED, &[]);
                assert!(result.is_ok() || vm.interrupt_requested());
            }
        }
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AbortSignal {
    event_target: CppEventTarget,
    abort_reason: Cell<Value>,
    // NB: The identifiers increase, so the list stays sorted by them.
    abort_algorithms: GcRefCell<Vec<(u64, AbortAlgorithm)>>,
    next_abort_algorithm_id: Cell<u64>,
    // AD-HOC: Source and dependent signals are strongly traced.
    source_signals: GcRefCell<Vec<Ref<AbortSignal>>>,
    dependent_signals: GcRefCell<Vec<Ref<AbortSignal>>>,
    dependent: Cell<bool>,
}

impl Wrappable for AbortSignal {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }

    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        self.abort_algorithms
            .borrow()
            .capacity()
            .saturating_mul(size_of::<(u64, AbortAlgorithm)>())
            .saturating_add(
                self.source_signals
                    .borrow()
                    .capacity()
                    .saturating_mul(size_of::<Ref<Self>>()),
            )
            .saturating_add(
                self.dependent_signals
                    .borrow()
                    .capacity()
                    .saturating_mul(size_of::<Ref<Self>>()),
            )
    }
}

impl GetTheParent for AbortSignal {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl AbortSignal {
    pub(crate) fn create(vm: &Vm) -> Ref<Self> {
        let signal = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            abort_reason: Cell::new(Value::UNDEFINED),
            abort_algorithms: GcRefCell::default(),
            next_abort_algorithm_id: Cell::new(0),
            source_signals: GcRefCell::default(),
            dependent_signals: GcRefCell::default(),
            dependent: Cell::new(false),
        });
        signal.event_target.set_owner(signal);
        signal
    }

    pub(crate) fn aborted(&self) -> bool {
        !self.abort_reason.get().is_undefined()
    }

    pub(crate) fn reason(&self) -> Value {
        self.abort_reason.get()
    }

    // https://dom.spec.whatwg.org/#abortsignal-add
    pub(crate) fn add_abort_algorithm(&self, vm: &Vm, abort_algorithm: AbortAlgorithm) -> Option<u64> {
        // 1. If signal is aborted, then return.
        if self.aborted() {
            return None;
        }

        // 2. Append algorithm to signal’s abort algorithms.
        let id = self.next_abort_algorithm_id.get().wrapping_add(1);
        self.next_abort_algorithm_id.set(id);
        self.abort_algorithms.borrow_mut(vm).push((id, abort_algorithm));
        Some(id)
    }

    // https://dom.spec.whatwg.org/#abortsignal-remove
    pub(crate) fn remove_abort_algorithm(&self, vm: &Vm, id: u64) {
        // To remove an algorithm algorithm from an AbortSignal signal, remove algorithm from signal’s abort algorithms.
        let mut algorithms = self.abort_algorithms.borrow_mut(vm);
        if let Ok(index) = algorithms.binary_search_by_key(&id, |(algorithm_id, _)| *algorithm_id) {
            algorithms.remove(index);
        }
    }

    // https://dom.spec.whatwg.org/#abortsignal-signal-abort
    pub(crate) fn signal_abort(&self, vm: &Vm, reason: Option<Value>, realm: Gc<Realm>) {
        // 1. If signal is aborted, then return.
        if self.aborted() {
            return;
        }

        // 2. Set signal’s abort reason to reason if it is given; otherwise to a new "AbortError" DOMException.
        let reason = match reason {
            Some(reason) if !reason.is_undefined() => reason,
            _ => exception_value(
                vm,
                realm,
                Exception::Dom("AbortError", Utf16String::from_ascii(b"Aborted without reason")),
            ),
        };
        self.abort_reason.set(reason);

        // 3. Let dependentSignalsToAbort be a new list.
        let dependent_signals_to_abort = MarkedVec::new(vm);

        // 4. For each dependentSignal of signal’s dependent signals:
        for dependent_signal in self.dependent_signals.borrow().iter() {
            // 1. If dependentSignal is not aborted:
            if !dependent_signal.aborted() {
                // 1. Set dependentSignal’s abort reason to signal’s abort reason.
                dependent_signal.abort_reason.set(self.abort_reason.get());

                // 2. Append dependentSignal to dependentSignalsToAbort.
                dependent_signals_to_abort.push(*dependent_signal);
            }
        }

        // 5. Run the abort steps for signal.
        self.run_the_abort_steps(vm, realm);

        // 6. For each dependentSignal of dependentSignalsToAbort, run the abort steps for dependentSignal.
        for dependent_signal in dependent_signals_to_abort.values().iter() {
            dependent_signal.run_the_abort_steps(vm, realm);
        }
    }

    // https://dom.spec.whatwg.org/#run-the-abort-steps
    fn run_the_abort_steps(&self, vm: &Vm, realm: Gc<Realm>) {
        // 1. For each algorithm of signal’s abort algorithms: run algorithm.
        // NB: An algorithm can remove algorithms, including itself, so this looks up the next one after each runs.
        let mut previous = None;
        loop {
            let next = {
                let algorithms = self.abort_algorithms.borrow();
                let index = algorithms.partition_point(|(id, _)| previous.is_some_and(|previous| *id <= previous));
                algorithms.get(index).cloned()
            };
            let Some((id, algorithm)) = next else {
                break;
            };
            algorithm.run(vm);
            previous = Some(id);
        }

        // 2. Empty signal’s abort algorithms.
        self.abort_algorithms.borrow_mut(vm).clear();

        // 3. Fire an event named abort at signal.
        self.event_target
            .dispatch_event(&CppEvent::create(realm, "abort", false, false, false));
    }

    // https://dom.spec.whatwg.org/#dom-abortsignal-throwifaborted
    pub(crate) fn throw_if_aborted(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // The throwIfAborted() method steps are to throw this’s abort reason, if this is aborted.
        if self.aborted() {
            return Err(Exception::from(Throw::new(self.abort_reason.get())));
        }
        Ok(())
    }

    // https://dom.spec.whatwg.org/#dom-abortsignal-abort
    pub(crate) fn abort(cx: &mut CallContext, maybe_reason: Option<Value>) -> WebIdlResult<Ref<Self>> {
        // 1. Let signal be a new AbortSignal object.
        let signal = Self::create(cx.vm());

        // 2. Set signal’s abort reason to reason if it is given; otherwise to a new "AbortError" DOMException.
        let mut reason = maybe_reason.unwrap_or(Value::UNDEFINED);
        if reason.is_undefined() {
            reason = exception_value(
                cx.vm(),
                cx.realm(),
                Exception::Dom("AbortError", Utf16String::from_ascii(b"Aborted without reason")),
            );
        }
        signal.abort_reason.set(reason);

        // 3. Return signal.
        Ok(signal)
    }

    // https://dom.spec.whatwg.org/#dom-abortsignal-timeout
    pub(crate) fn timeout(cx: &mut CallContext, milliseconds: u64) -> WebIdlResult<Ref<Self>> {
        // 1. Let signal be a new AbortSignal object.
        let signal = Self::create(cx.vm());

        // 2. Let global be signal’s relevant global object.
        let realm = cx.realm();
        let global = realm.global_object();

        // 3. Run steps after a timeout given global, "AbortSignal-timeout", milliseconds, and the following step:
        let global_scope = window_or_worker_global_scope_from_global_object(global)
            .expect("AbortSignal.timeout() is only exposed on a Window or WorkerGlobalScope");
        global_scope.run_steps_after_a_timeout(cx.vm(), milliseconds, (realm, signal), |vm, (realm, signal)| {
            // 1. Queue a global task on the timer task source given global to signal abort given signal and a new "TimeoutError" DOMException.
            queue_a_global_task(
                vm,
                realm.global_object(),
                TaskSource::TimerTask,
                (realm, signal),
                |vm, (realm, signal)| {
                    let reason = exception_value(
                        vm,
                        realm,
                        Exception::Dom("TimeoutError", Utf16String::from_ascii(b"Signal timed out")),
                    );
                    signal.signal_abort(vm, Some(reason), realm);
                },
            );
        });

        // 4. Return signal.
        Ok(signal)
    }

    // https://dom.spec.whatwg.org/#dom-abortsignal-any
    pub(crate) fn any(cx: &mut CallContext, signals: MarkedVec<'_, Ref<Self>>) -> WebIdlResult<Ref<Self>> {
        // The static any(signals) method steps are to return the result of creating a dependent abort signal from signals using AbortSignal and the current realm.
        Ok(Self::create_dependent_abort_signal(cx.vm(), &signals.values()))
    }

    // https://dom.spec.whatwg.org/#dom-abortsignal-aborted
    pub(crate) fn get_aborted(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The aborted getter steps are to return true if this is aborted; otherwise false.
        Ok(self.aborted())
    }

    // https://dom.spec.whatwg.org/#dom-abortsignal-reason
    pub(crate) fn get_reason(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        // The reason getter steps are to return this’s abort reason.
        Ok(self.reason())
    }

    // https://dom.spec.whatwg.org/#create-a-dependent-abort-signal
    pub(crate) fn create_dependent_abort_signal(vm: &Vm, signals: &[Ref<Self>]) -> Ref<Self> {
        // 1. Let resultSignal be a new object implementing signalInterface using realm.
        let result_signal = Self::create(vm);

        // 2. For each signal of signals: if signal is aborted, then set resultSignal’s abort reason to signal’s abort reason and return resultSignal.
        for signal in signals {
            if signal.aborted() {
                result_signal.abort_reason.set(signal.reason());
                return result_signal;
            }
        }

        // 3. Set resultSignal’s dependent to true.
        result_signal.dependent.set(true);

        // 4. For each signal of signals:
        for signal in signals {
            // 1. If signal’s dependent is false:
            if !signal.dependent.get() {
                // 1. Append signal to resultSignal’s source signals.
                result_signal.source_signals.borrow_mut(vm).push(*signal);

                // 2. Append resultSignal to signal’s dependent signals.
                signal.dependent_signals.borrow_mut(vm).push(result_signal);
            }
            // 2. Otherwise, for each sourceSignal of signal’s source signals:
            else {
                for source_signal in signal.source_signals.borrow().iter() {
                    // 1. Assert: sourceSignal is not aborted and not dependent.
                    assert!(!source_signal.aborted());
                    assert!(!source_signal.dependent.get());

                    // 2. Append sourceSignal to resultSignal’s source signals.
                    result_signal.source_signals.borrow_mut(vm).push(*source_signal);

                    // 3. Append resultSignal to sourceSignal’s dependent signals.
                    source_signal.dependent_signals.borrow_mut(vm).push(result_signal);
                }
            }
        }

        // 5. Return resultSignal.
        result_signal
    }
}

/// Creates an AbortSignal.
///
/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_abort_signal_create(vm: &Vm) -> *mut c_void {
    cell_into_abi(AbortSignal::create(vm))
}

/// Returns whether the signal is aborted.
///
/// # Safety
///
/// `signal` must be a live AbortSignal cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_abort_signal_aborted(signal: *mut c_void) -> bool {
    // SAFETY: The caller passes a live AbortSignal.
    unsafe { cell_from_abi::<AbortSignal>(signal) }.aborted()
}

/// Returns the signal’s abort reason.
///
/// # Safety
///
/// `signal` must be a live AbortSignal cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_abort_signal_reason(signal: *mut c_void) -> u64 {
    // SAFETY: The caller passes a live AbortSignal.
    unsafe { cell_from_abi::<AbortSignal>(signal) }.reason().to_raw()
}

/// Adds a C++ abort algorithm and returns its identifier if the signal accepts it.
///
/// # Safety
///
/// `vm`, `signal`, and `algorithm` must be live, and `algorithm` a GC::Function<void()>.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_abort_signal_add_abort_algorithm(
    vm: &Vm,
    signal: *mut c_void,
    algorithm: *mut c_void,
    id: &mut u64,
) -> bool {
    // SAFETY: The caller passes live cells of the declared types.
    let signal = unsafe { cell_from_abi::<AbortSignal>(signal) };
    let algorithm = AbortAlgorithm::Cpp(unsafe { CppCell::from_raw(algorithm) });
    let Some(algorithm_id) = signal.add_abort_algorithm(vm, algorithm) else {
        return false;
    };
    *id = algorithm_id;
    true
}

/// Removes the abort algorithm with the identifier.
///
/// # Safety
///
/// `vm` and `signal` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_abort_signal_remove_abort_algorithm(vm: &Vm, signal: *mut c_void, id: u64) {
    // SAFETY: The caller passes a live AbortSignal.
    unsafe { cell_from_abi::<AbortSignal>(signal) }.remove_abort_algorithm(vm, id);
}

/// Signals abort with the reason in the realm.
///
/// # Safety
///
/// `vm`, `signal`, `reason`, and `realm` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_abort_signal_signal_abort(
    vm: *mut c_void,
    signal: *mut c_void,
    reason: u64,
    realm: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, realm, signal, and reason.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let signal = unsafe { cell_from_abi::<AbortSignal>(signal) };
    signal.signal_abort(vm, Some(unsafe { Value::from_raw(reason) }), realm);
}

/// Creates a dependent AbortSignal from the signals.
///
/// # Safety
///
/// `vm` and every cell in `signals` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_abort_signal_create_dependent_abort_signal(
    vm: &Vm,
    signals: ak::Span<'_, *mut c_void>,
) -> *mut c_void {
    let list = MarkedVec::with_capacity(vm, signals.as_slice().len());
    for &signal in signals.as_slice() {
        // SAFETY: The caller passes live AbortSignal cells.
        list.push(unsafe { cell_from_abi::<AbortSignal>(signal) });
    }
    cell_into_abi(AbortSignal::create_dependent_abort_signal(vm, &list.values()))
}
