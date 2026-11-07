/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The threads behind $262.agent, as test262's INTERPRETING.md describes it.
//!
//! Every agent a test starts is a thread with a VM of its own, in the process of the test, as agents that share memory
//! have to be: Atomics.wait() and Atomics.notify() meet in the waiter lists of the process. The test hands an agent its
//! source, and then the broadcasts. Reports from all of a test's agents share one queue, so the test reads them in the
//! order the agents made them. Ending the test interrupts its agents and joins their threads, so no agent outlives the
//! test that started it.

use std::cell::RefCell;
use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::thread::JoinHandle;

use crate::interpreter::vm::Vm;
use crate::runtime::shared_backing_store::SharedBackingStore;

/// JS::VM::MINIMUM_THREAD_STACK_SIZE, which the thread of a VM needs.
const AGENT_THREAD_STACK_SIZE: usize = 8 * 1024 * 1024;

/// The number that $262.agent.broadcast() passes along with its SharedArrayBuffer.
#[derive(Clone, Debug, PartialEq)]
pub enum BroadcastNumber {
    Undefined,
    Number(f64),
    /// The two's complement bytes of the BigInt, least significant first.
    BigInt(Vec<u8>),
}

/// A broadcast as an agent receives it: the store of the SharedArrayBuffer, and the number.
#[derive(Clone)]
pub struct Broadcast {
    pub store: Arc<SharedBackingStore>,
    pub number: BroadcastNumber,
}

/// Why an agent stopped before it sent the report that the test waits for.
#[derive(Clone, Debug, PartialEq)]
pub enum AgentFailure {
    UncaughtException { name: String, message: String },
}

/// Which half of $262.agent this thread provides.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum AgentRole {
    /// Neither: the tool did not set up agents.
    Unavailable,
    /// The test's: start(), broadcast() and getReport().
    Test,
    /// An agent's: receiveBroadcast(), report() and leaving().
    Agent,
}

pub fn role() -> AgentRole {
    if THIS_AGENT.with_borrow(Option::is_some) {
        return AgentRole::Agent;
    }
    if AGENT_ENTRY.get().is_some() {
        return AgentRole::Test;
    }
    AgentRole::Unavailable
}

/// What the thread of an agent runs, with the WTF-8 source of the agent's script.
pub type AgentEntry = fn(&[u8]);

thread_local! {
    static AGENT_ENTRY: core::cell::Cell<Option<AgentEntry>> = const { core::cell::Cell::new(None) };
    static RUNNING_AGENTS: RefCell<RunningAgents> = RefCell::new(RunningAgents::default());
    static THIS_AGENT: RefCell<Option<Arc<Agent>>> = const { RefCell::new(None) };
}

/// Lets tests on this thread start agents, each of which runs `entry` on a thread of its own.
pub fn enable_agents(entry: AgentEntry) {
    AGENT_ENTRY.set(Some(entry));
}

/// What the agents of a test report, in the order they did.
#[derive(Default)]
struct Reports {
    reports: VecDeque<Vec<u8>>,
    failures: Vec<AgentFailure>,
}

#[derive(Default)]
struct AgentState {
    broadcasts: VecDeque<Broadcast>,
    is_running: bool,
    has_finished: bool,
    is_terminated: bool,
    /// The VM of the agent's thread, for as long as the thread has one.
    vm: Option<*const Vm>,
}

// SAFETY: The VM pointer goes away under the lock before its VM does, and is only used to call Vm::request_interrupt(),
//         which any thread may call.
unsafe impl Send for AgentState {}

/// The state a test and one of its agents share.
struct Agent {
    state: Mutex<AgentState>,
    state_changed: Condvar,
    reports: Arc<Mutex<Reports>>,
}

impl Agent {
    fn update<R>(&self, update: impl FnOnce(&mut AgentState) -> R) -> R {
        let result = update(&mut self.state.lock().unwrap_or_else(PoisonError::into_inner));
        self.state_changed.notify_all();
        result
    }

    fn wait_until_running(&self) {
        let mut state = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        while !state.is_running && !state.has_finished {
            state = self.state_changed.wait(state).unwrap_or_else(PoisonError::into_inner);
        }
    }

    fn terminate(&self) {
        self.update(|state| {
            state.is_terminated = true;
            if let Some(vm) = state.vm {
                // SAFETY: The agent's thread clears the pointer under the lock this holds before the VM goes away.
                unsafe { (*vm).request_interrupt() };
            }
        });
    }
}

/// An agent as the test sees it.
struct RunningAgent {
    agent: Arc<Agent>,
    thread: JoinHandle<()>,
}

/// The agents that the running test started.
#[derive(Default)]
struct RunningAgents {
    agents: Vec<RunningAgent>,
    reports: Arc<Mutex<Reports>>,
    /// How many of the failures in the reports getReport() has thrown.
    thrown_failure_count: usize,
}

impl RunningAgents {
    fn start(&mut self, entry: AgentEntry, source: &[u8]) -> std::io::Result<()> {
        let agent = Arc::new(Agent {
            state: Mutex::default(),
            state_changed: Condvar::new(),
            reports: Arc::clone(&self.reports),
        });
        let agent_of_thread = Arc::clone(&agent);
        let source = source.to_vec();
        let thread = std::thread::Builder::new()
            .name("test262 agent".to_string())
            .stack_size(AGENT_THREAD_STACK_SIZE)
            .spawn(move || {
                THIS_AGENT.set(Some(Arc::clone(&agent_of_thread)));
                entry(&source);
                THIS_AGENT.set(None);
                agent_of_thread.update(|state| state.has_finished = true);
            })?;
        agent.wait_until_running();
        self.agents.push(RunningAgent { agent, thread });
        Ok(())
    }

    fn broadcast(&mut self, broadcast: &Broadcast) {
        for agent in &self.agents {
            // An agent that already exited cannot take the message, and the test cannot expect it to.
            agent.agent.update(|state| {
                if !state.has_finished {
                    state.broadcasts.push_back(broadcast.clone());
                }
            });
        }
    }

    fn take_report(&mut self) -> Result<Option<Vec<u8>>, AgentFailure> {
        let mut reports = self.reports.lock().unwrap_or_else(PoisonError::into_inner);
        if let Some(report) = reports.reports.pop_front() {
            return Ok(Some(report));
        }

        // With nothing to report, tell the test whether an agent failed, which would leave it waiting forever.
        let Some(failure) = reports.failures.get(self.thrown_failure_count) else {
            return Ok(None);
        };
        self.thrown_failure_count += 1;
        Err(failure.clone())
    }
}

/// $262.agent.start(): runs `source`, the WTF-8 of a script, in a new agent and returns once the agent is running.
pub fn start_agent(source: &[u8]) -> Result<(), String> {
    let entry = AGENT_ENTRY.get().ok_or("Agents are unavailable")?;
    RUNNING_AGENTS
        .with_borrow_mut(|running_agents| running_agents.start(entry, source))
        .map_err(|error| format!("Could not start an agent: {error}"))
}

/// $262.agent.broadcast(): hands a SharedArrayBuffer's store and a number to every running agent, and returns once each
/// has them.
pub fn broadcast(broadcast: &Broadcast) {
    RUNNING_AGENTS.with_borrow_mut(|running_agents| running_agents.broadcast(broadcast));
}

/// $262.agent.getReport(): the next report of any agent, which is WTF-8, or None if there is none yet. Fails instead of
/// returning None when an agent stopped short, as no report might come from it.
pub fn take_report() -> Result<Option<Vec<u8>>, AgentFailure> {
    RUNNING_AGENTS.with_borrow_mut(RunningAgents::take_report)
}

/// Ends the agents of the test that ran last, and returns how each one that failed did, whether getReport() threw it
/// or not.
pub fn terminate_agents() -> Vec<AgentFailure> {
    let running_agents = RUNNING_AGENTS.take();
    let failures = core::mem::take(
        &mut running_agents
            .reports
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .failures,
    );
    for running_agent in &running_agents.agents {
        running_agent.agent.terminate();
    }
    for running_agent in running_agents.agents {
        let _ = running_agent.thread.join();
    }
    failures
}

fn this_agent() -> Arc<Agent> {
    THIS_AGENT.with_borrow(Clone::clone).expect("this thread runs an agent")
}

/// Tells the test that this agent is running, on `vm`, which ends $262.agent.start(). The agent must call
/// agent_vm_went_away() before the VM goes.
pub fn acknowledge_start(vm: &Vm) {
    this_agent().update(|state| {
        state.is_running = true;
        state.vm = Some(core::ptr::from_ref(vm));
        if state.is_terminated {
            vm.request_interrupt();
        }
    });
}

/// Keeps the test from reaching the VM of this agent, which is about to go.
pub fn agent_vm_went_away() {
    this_agent().update(|state| state.vm = None);
}

/// $262.agent.receiveBroadcast(): waits for the next broadcast, or None once the test ended.
pub fn receive_broadcast() -> Option<Broadcast> {
    let agent = this_agent();
    let mut state = agent.state.lock().unwrap_or_else(PoisonError::into_inner);
    loop {
        if state.is_terminated {
            return None;
        }
        if let Some(broadcast) = state.broadcasts.pop_front() {
            return Some(broadcast);
        }
        state = agent.state_changed.wait(state).unwrap_or_else(PoisonError::into_inner);
    }
}

/// $262.agent.report(), with the WTF-8 of the reported string.
pub fn report(message: &[u8]) {
    this_agent()
        .reports
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
        .reports
        .push_back(message.to_vec());
}

/// Tells the test that the agent's script threw, or left a promise rejected without a handler.
pub fn report_uncaught_exception(name: &str, message: &str) {
    this_agent()
        .reports
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
        .failures
        .push(AgentFailure::UncaughtException {
            name: name.to_string(),
            message: message.to_string(),
        });
}
