/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::sync::atomic::{AtomicPtr, Ordering};
use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::thread::ThreadId;
use std::time::{Duration, Instant};

use ak::waiter_list::{self, AsyncWaiter, AtomicWaitResult};
use libjs_runtime_macros::Trace;

use crate::embedding::async_wait::AsyncWaitHooks;
use crate::gc::heap_function::HeapFunction;
use crate::gc::sweep::Sweep;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::array_buffer::ArrayBuffer;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise_capability::new_promise_capability;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

pub(crate) struct PendingWait {
    pub id: u64,
    pub resolve: Gc<FunctionObject>,
    pub realm: Gc<Realm>,
    pub buffer: Gc<ArrayBuffer>,
    pub waiter: Arc<AsyncWaiter>,
    pub deadline: Option<Instant>,
    pub result: Option<AtomicWaitResult>,
    pub host_timer: Option<i32>,
}

#[derive(Trace)]
pub(crate) struct TimeoutJob {
    #[gc(untraced)]
    pub deadline: Instant,
    pub callback: Gc<HeapFunction>,
}

// SAFETY: The resolver and buffer are reachable while the realm is. The sweep callback removes waits whose realm
// died, before any of their cells can be freed. The native waiter never dereferences a GC pointer.
unsafe impl Trace for PendingWait {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_ephemeron(self.realm, &Value::from_object(self.resolve));
        visitor.visit_ephemeron(self.realm, &Value::from_object(self.buffer));
    }
}

/// Only completion identifiers cross threads. The VM pointer can only be dereferenced on its owning thread, and is
/// cleared on that thread before the VM's heap is destroyed. A queued host callback keeps this record, not the VM.
pub struct AsyncWaitAgent {
    vm: AtomicPtr<Vm>,
    owner: ThreadId,
    ready: Mutex<VecDeque<(u64, AtomicWaitResult)>>,
    changed: Condvar,
    pub(crate) hooks: Mutex<Option<Arc<AsyncWaitHooks>>>,
}

impl AsyncWaitAgent {
    pub(crate) fn new(vm: &Vm) -> Arc<Self> {
        Arc::new(Self {
            vm: AtomicPtr::new(core::ptr::from_ref(vm).cast_mut()),
            owner: std::thread::current().id(),
            ready: Mutex::new(VecDeque::new()),
            changed: Condvar::new(),
            hooks: Mutex::new(None),
        })
    }

    pub(crate) fn host_hooks(&self) -> Option<Arc<AsyncWaitHooks>> {
        self.hooks.lock().unwrap_or_else(PoisonError::into_inner).clone()
    }

    fn complete(self: &Arc<Self>, id: u64, result: AtomicWaitResult) {
        if self.owner == std::thread::current().id() {
            let vm = self.vm.load(Ordering::Acquire);
            if vm.is_null() {
                return;
            }
            // SAFETY: The owner clears this pointer before destroying its VM.
            let vm = unsafe { &*vm };
            {
                let mut pending = vm.async_waits().borrow_mut(vm);
                let Some(wait) = pending.iter_mut().find(|wait| wait.id == id) else {
                    return;
                };
                wait.result = Some(result);
            }
            vm.resolve_async_wait(id);
            return;
        }
        self.ready
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .push_back((id, result));
        self.changed.notify_all();
        if let Some(hooks) = self.host_hooks() {
            hooks.wake(Arc::clone(self));
        }
    }

    pub fn dispatch(&self) {
        assert_eq!(self.owner, std::thread::current().id());
        let vm = self.vm.load(Ordering::Acquire);
        if vm.is_null() {
            return;
        }
        // SAFETY: Only the owning thread reads the VM, and it clears this pointer before destroying it.
        let vm = unsafe { &*vm };
        loop {
            let Some((id, result)) = self.ready.lock().unwrap_or_else(PoisonError::into_inner).pop_front() else {
                break;
            };
            let realm = {
                let mut pending = vm.async_waits().borrow_mut(vm);
                let Some(wait) = pending.iter_mut().find(|wait| wait.id == id) else {
                    continue;
                };
                wait.result = Some(result);
                wait.realm
            };
            // https://tc39.es/ecma262/#sec-notifywaiter
            if let Some(hooks) = self.host_hooks() {
                // 4.a. Perform EnqueueResolveInAgentJob(waiterRecord.[[AgentSignifier]], waiterRecord.[[PromiseCapability]], waiterRecord.[[Result]]).
                hooks.enqueue_resolution(vm, id, realm);
            } else {
                vm.resolve_async_wait(id);
            }
        }
    }

    pub(crate) fn wake(&self) {
        let _ready = self.ready.lock().unwrap_or_else(PoisonError::into_inner);
        self.changed.notify_all();
    }

    pub(crate) fn close(&self) {
        self.vm.store(core::ptr::null_mut(), Ordering::Release);
        self.hooks.lock().unwrap_or_else(PoisonError::into_inner).take();
        self.changed.notify_all();
    }
}

impl Vm {
    pub(crate) fn enqueue_timeout_job(&self, callback: Gc<HeapFunction>, delay: Duration) {
        let now = Instant::now();
        self.timeout_jobs().borrow_mut(self).push(TimeoutJob {
            deadline: now.checked_add(delay).unwrap_or(now),
            callback,
        });
    }

    fn run_ready_timeout_job(&self) -> ThrowCompletionOr<()> {
        let callback = {
            let mut jobs = self.timeout_jobs().borrow_mut(self);
            let Some(index) = jobs
                .iter()
                .enumerate()
                .min_by_key(|(_, job)| job.deadline)
                .map(|(index, _)| index)
            else {
                return Ok(());
            };
            if jobs[index].deadline > Instant::now() {
                return Ok(());
            }
            jobs.remove(index).callback
        };
        HeapFunction::call(callback, self).map(|_| ())
    }

    pub(crate) fn remove_dead_async_waits(&self, sweep: &Sweep<'_>) {
        self.async_waits().borrow_mut_in_sweep(sweep).retain(|wait| {
            if !sweep.is_dead(wait.realm) {
                return true;
            }
            waiter_list::cancel_async_waiter(&wait.waiter);
            false
        });
    }

    pub(crate) fn async_wait_agent(&self) -> &Arc<AsyncWaitAgent> {
        self.async_wait_agent.get_or_init(|| AsyncWaitAgent::new(self))
    }

    pub(crate) fn start_async_wait(
        &self,
        buffer: Gc<ArrayBuffer>,
        byte_index: usize,
        expected: u64,
        element_size: usize,
        timeout: f64,
    ) -> Value {
        let realm = self.current_realm().expect("an atomic wait has a current realm");
        // https://tc39.es/ecma262/#sec-dowait
        // 16.a. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let capability =
            new_promise_capability(self, Value::from_object(realm.intrinsics().promise_constructor(self))).must();
        // 16.b. Let resultObj be OrdinaryObjectCreate(%Object.prototype%).
        let object = Object::create(self, realm, Some(realm.intrinsics().object_prototype(self).upcast()));
        let agent = Arc::clone(self.async_wait_agent());
        let id = self.next_async_wait_id.get();
        self.next_async_wait_id
            .set(id.checked_add(1).expect("async wait identifiers fit in u64"));
        let weak_agent = Arc::downgrade(&agent);
        let waiter = AsyncWaiter::new(move |result| {
            if let Some(agent) = weak_agent.upgrade() {
                agent.complete(id, result);
            }
        });
        let deadline = Duration::try_from_secs_f64(timeout / 1000.0)
            .ok()
            .and_then(|duration| Instant::now().checked_add(duration));
        self.async_waits().borrow_mut(self).push(PendingWait {
            id,
            resolve: capability.resolve(),
            realm,
            buffer,
            waiter: Arc::clone(&waiter),
            deadline,
            result: None,
            host_timer: None,
        });
        // NB: The shared waiter list combines steps 17-21 and 28. Its lock covers both the value comparison and
        //     registration, so a notification cannot pass between them.
        let immediate = buffer.atomic_wait_async(byte_index, expected, element_size, timeout == 0.0, &waiter);
        let value = if let Some(result) = immediate {
            self.async_waits().borrow_mut(self).retain(|wait| wait.id != id);
            Value::from_string(PrimitiveString::create_from_utf8(
                self,
                if result == AtomicWaitResult::NotEqual {
                    "not-equal"
                } else {
                    "timed-out"
                },
            ))
        } else {
            // 30. Else if timeoutTime is finite, then
            if let Some(deadline) = deadline
                && let Some(hooks) = agent.host_hooks()
            {
                // a. Perform EnqueueAtomicsWaitAsyncTimeoutJob(waiterList, waiterRecord).
                let timer = hooks.schedule_timeout(
                    self,
                    id,
                    realm,
                    deadline.saturating_duration_since(Instant::now()).as_secs_f64() * 1000.0,
                );
                if let Some(wait) = self
                    .async_waits()
                    .borrow_mut(self)
                    .iter_mut()
                    .find(|wait| wait.id == id)
                {
                    wait.host_timer = Some(timer);
                }
            }
            Value::from_object(capability.promise())
        };
        // 33. Perform ! CreateDataPropertyOrThrow(resultObj, "async", true).
        // 34. Perform ! CreateDataPropertyOrThrow(resultObj, "value", promiseCapability.[[Promise]]).
        // NB: Steps 20 and 21 use the same properties with false and an immediate string result.
        object
            .create_data_property_or_throw(
                self,
                &PropertyKey::from_utf8("async"),
                Value::from_bool(immediate.is_none()),
            )
            .must();
        object
            .create_data_property_or_throw(self, &PropertyKey::from_utf8("value"), value)
            .must();
        // 35. Return resultObj.
        Value::from_object(object)
    }

    pub fn resolve_async_wait(&self, id: u64) {
        let wait = {
            let mut waits = self.async_waits().borrow_mut(self);
            let Some(index) = waits.iter().position(|wait| wait.id == id && wait.result.is_some()) else {
                return;
            };
            waits.remove(index)
        };
        if let Some(timer) = wait.host_timer
            && let Some(hooks) = self.async_wait_agent().host_hooks()
        {
            hooks.cancel_timeout(self, wait.realm, timer);
        }
        let result = PrimitiveString::create_from_utf8(
            self,
            if wait.result == Some(AtomicWaitResult::Woken) {
                "ok"
            } else {
                "timed-out"
            },
        );
        // https://tc39.es/ecma262/#sec-notifywaiter
        // 3.b. Perform ! Call(promiseCapability.[[Resolve]], undefined, « waiterRecord.[[Result]] »).
        call_function_object(self, wait.resolve, Value::UNDEFINED, &[Value::from_string(result)]).must();
    }

    pub fn timeout_async_wait(&self, id: u64) {
        let wait = self
            .async_waits()
            .borrow()
            .iter()
            .find(|wait| wait.id == id)
            .map(|wait| (Arc::clone(&wait.waiter), wait.deadline, wait.realm));
        let Some((waiter, Some(deadline), realm)) = wait else {
            return;
        };
        if deadline > Instant::now() {
            if let Some(hooks) = self.async_wait_agent().host_hooks() {
                let timer = hooks.schedule_timeout(
                    self,
                    id,
                    realm,
                    deadline.saturating_duration_since(Instant::now()).as_secs_f64() * 1000.0,
                );
                if let Some(wait) = self
                    .async_waits()
                    .borrow_mut(self)
                    .iter_mut()
                    .find(|wait| wait.id == id)
                {
                    wait.host_timer = Some(timer);
                }
            }
            return;
        }
        // https://tc39.es/ecma262/#sec-enqueueatomicswaitasynctimeoutjob
        // 1.b. If waiterList.[[Waiters]] contains waiterRecord, then
        // iv. Perform RemoveWaiter(waiterList, waiterRecord).
        // v. Perform NotifyWaiter(waiterList, waiterRecord).
        waiter_list::timeout_async_waiter(&waiter);
    }

    /// Completes the waits that are ready: those notified from another agent, and those whose timeout expired.
    fn complete_ready_async_waits(&self, agent: &AsyncWaitAgent) {
        agent.dispatch();
        let expired = self
            .async_waits()
            .borrow()
            .iter()
            .filter(|wait| wait.deadline.is_some_and(|deadline| deadline <= Instant::now()))
            .map(|wait| wait.id)
            .collect::<Vec<_>>();
        for id in expired {
            self.timeout_async_wait(id);
        }
    }

    /// Runs the promise jobs queued, and completes the waits that are ready, without sleeping for the others: a
    /// REPL line returns to the prompt, from which a later line may notify them.
    pub fn run_ready_async_wait_jobs(&self) {
        self.run_queued_promise_jobs();
        if self.async_waits().borrow().is_empty() {
            return;
        }
        let agent = Arc::clone(self.async_wait_agent());
        self.complete_ready_async_waits(&agent);
        self.run_queued_promise_jobs();
    }

    /// The standalone host runs tasks between promise checkpoints, sleeping until notification or the next timeout.
    pub fn run_async_wait_jobs_until(&self, done: impl Fn() -> bool) -> ThrowCompletionOr<()> {
        self.run_queued_promise_jobs();
        if done() || (self.async_waits().borrow().is_empty() && self.timeout_jobs().borrow().is_empty()) {
            return Ok(());
        }
        let agent = Arc::clone(self.async_wait_agent());
        while !done() && !self.interrupt_requested() {
            self.complete_ready_async_waits(&agent);
            self.run_queued_promise_jobs();
            if done() {
                break;
            }
            self.run_ready_timeout_job()?;
            self.run_queued_promise_jobs();
            if done() || (self.async_waits().borrow().is_empty() && self.timeout_jobs().borrow().is_empty()) {
                break;
            }
            let next = self
                .async_waits()
                .borrow()
                .iter()
                .filter_map(|wait| wait.deadline)
                .chain(self.timeout_jobs().borrow().iter().map(|job| job.deadline))
                .min();
            let ready = agent.ready.lock().unwrap_or_else(PoisonError::into_inner);
            if !ready.is_empty() {
                continue;
            }
            if self.interrupt_requested() {
                break;
            }
            if let Some(deadline) = next {
                drop(
                    agent
                        .changed
                        .wait_timeout(ready, deadline.saturating_duration_since(Instant::now()))
                        .unwrap_or_else(PoisonError::into_inner),
                );
            } else {
                drop(agent.changed.wait(ready).unwrap_or_else(PoisonError::into_inner));
            }
        }
        Ok(())
    }
}
