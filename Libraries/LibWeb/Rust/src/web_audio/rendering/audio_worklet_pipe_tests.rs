/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, Weak};
use std::thread;
use std::time::Duration;

use super::audio_worklet_pipe::{
    AudioWorkletPipe, AudioWorkletPipeClient, Config, WorkletPipeState, ring_sizing_for_device_latency,
};

const QUANTUM_SIZE: usize = 128;
const RING_CAPACITY: usize = 64;
const PRIME_LEVEL: usize = 48;
const BURST_QUANTA: usize = 38;

struct PostedTask {
    task: usize,
    invoke: unsafe extern "C" fn(*mut c_void),
}

type EventLoop = Mutex<VecDeque<PostedTask>>;

thread_local! {
    static EVENT_LOOP: &'static EventLoop = Box::leak(Box::default());
}

#[unsafe(no_mangle)]
extern "C" fn web_audio_current_event_loop() -> *mut c_void {
    EVENT_LOOP.with(|event_loop| std::ptr::from_ref::<EventLoop>(event_loop).cast_mut().cast())
}

#[unsafe(no_mangle)]
unsafe extern "C" fn web_audio_deferred_invoke(
    event_loop: *mut c_void,
    task: *mut c_void,
    invoke: unsafe extern "C" fn(*mut c_void),
) {
    let event_loop = unsafe { &*event_loop.cast::<EventLoop>() };
    event_loop.lock().unwrap().push_back(PostedTask {
        task: task as usize,
        invoke,
    });
}

fn run_posted_tasks() {
    loop {
        let task = EVENT_LOOP.with(|event_loop| event_loop.lock().unwrap().pop_front());
        let Some(task) = task else {
            return;
        };
        unsafe { (task.invoke)(task.task as *mut c_void) };
    }
}

fn test_pipe() -> Arc<AudioWorkletPipe> {
    AudioWorkletPipe::new(Config {
        quantum_size: QUANTUM_SIZE,
        input_count: 1,
        output_count: 1,
        input_channel_capacity: vec![2],
        output_channel_capacity: vec![2],
        output_channel_count_matches_input: false,
        param_count: 2,
        param_is_a_rate: vec![true, false],
        ring_capacity: RING_CAPACITY,
        prime_level: PRIME_LEVEL,
    })
}

struct InputSlotWriter<'a> {
    pipe: &'a AudioWorkletPipe,
    slot: *mut u8,
}

impl InputSlotWriter<'_> {
    fn set_start_frame(&mut self, frame: u64) {
        unsafe { AudioWorkletPipe::set_start_frame(self.slot, frame) };
    }

    fn set_had_gap(&mut self, had_gap: bool) {
        unsafe { AudioWorkletPipe::set_flag(self.slot, had_gap) };
    }

    fn set_actual_channel_count(&mut self, input: usize, channel_count: u32) {
        unsafe { *self.pipe.channel_count(self.slot, false, input) = channel_count };
    }

    fn input_channel(&mut self, input: usize, channel: usize) -> &mut [f32] {
        let (samples, length) = unsafe { self.pipe.channel(self.slot, false, input, channel) };
        unsafe { ak::slice_from_raw_parts_mut(samples, length) }
    }

    fn param_block(&mut self, param: usize) -> &mut [f32] {
        let (samples, length) = unsafe { self.pipe.param_block(self.slot, param) };
        unsafe { ak::slice_from_raw_parts_mut(samples, length) }
    }
}

struct OutputSlotReader<'a> {
    pipe: &'a AudioWorkletPipe,
    slot: *mut u8,
}

impl OutputSlotReader<'_> {
    fn start_frame(&self) -> u64 {
        unsafe { AudioWorkletPipe::start_frame(self.slot) }
    }

    fn processor_active(&self) -> bool {
        unsafe { AudioWorkletPipe::flag(self.slot) }
    }

    fn actual_channel_count(&self, output: usize) -> u32 {
        unsafe { *self.pipe.channel_count(self.slot, true, output) }
    }

    fn output_channel(&self, output: usize, channel: usize) -> &[f32] {
        let (samples, length) = unsafe { self.pipe.channel(self.slot, true, output, channel) };
        unsafe { ak::slice_from_raw_parts(samples, length) }
    }
}

fn try_push_input(pipe: &AudioWorkletPipe, fill: impl FnOnce(&mut InputSlotWriter<'_>)) -> bool {
    unsafe { pipe.try_push_input(|slot| fill(&mut InputSlotWriter { pipe, slot })) }
}

fn try_pop_output(pipe: &AudioWorkletPipe, read: impl FnOnce(&OutputSlotReader<'_>)) -> bool {
    unsafe { pipe.try_pop_output(|slot| read(&OutputSlotReader { pipe, slot })) }
}

fn random_sleep(state: &Cell<u32>) {
    let next = state.get().wrapping_mul(1_103_515_245).wrapping_add(12_345);
    state.set(next);
    thread::sleep(Duration::from_millis(u64::from((next >> 16) % 11)));
}

#[test]
fn ring_sizing_matches_renderer_defaults() {
    let sizing = ring_sizing_for_device_latency(100, 48000., 128);
    assert_eq!(sizing.ring_capacity, 64);
    assert_eq!(sizing.prime_level, 48);

    for latency_ms in [10_u32, 20, 50, 100, 200, 500] {
        let sizing = ring_sizing_for_device_latency(latency_ms, 48000., 128);
        let burst = (f64::from(latency_ms) / 1000. * 48000. / 128.).ceil() as usize;
        assert!(sizing.prime_level >= burst);
        assert!(sizing.ring_capacity > sizing.prime_level);
        assert!(sizing.ring_capacity.is_power_of_two());
    }
}

#[test]
fn pops_on_empty_rings_fail() {
    let pipe = test_pipe();

    assert!(!unsafe { pipe.try_read_input(|_| unreachable!()) });
    assert!(!try_pop_output(&pipe, |_| unreachable!()));

    // Only the audio thread's output pops count as underruns; an empty input ring just means the pump caught up.
    assert_eq!(pipe.underrun_count(), 1);
    assert_eq!(pipe.dropped_input_count(), 0);
    assert_eq!(pipe.output_occupancy(), 0);
}

#[test]
fn input_ring_roundtrip_fill_to_capacity_and_overrun() {
    let pipe = test_pipe();

    for i in 0..RING_CAPACITY {
        let pushed = try_push_input(&pipe, |slot| {
            slot.set_start_frame((i * QUANTUM_SIZE) as u64);
            if i == 3 {
                slot.set_had_gap(true);
            }
            slot.set_actual_channel_count(0, (i % 3) as u32);
            for channel in 0..i % 3 {
                let samples = slot.input_channel(0, channel);
                assert_eq!(samples.len(), QUANTUM_SIZE);
                for (frame, sample) in samples.iter_mut().enumerate() {
                    *sample = (i * 10 + channel) as f32 + frame as f32 * 0.25;
                }
            }
            assert_eq!(slot.param_block(0).len(), QUANTUM_SIZE);
            assert_eq!(slot.param_block(1).len(), 1);
            for (frame, sample) in slot.param_block(0).iter_mut().enumerate() {
                *sample = i as f32 + frame as f32 * 0.5;
            }
            slot.param_block(1)[0] = i as f32;
        });
        assert!(pushed);
    }

    assert!(!try_push_input(&pipe, |_| unreachable!()));
    assert_eq!(pipe.dropped_input_count(), 1);

    for i in 0..RING_CAPACITY {
        let popped = unsafe {
            pipe.try_read_input(|slot| {
                assert_eq!(slot.start_frame(), (i * QUANTUM_SIZE) as u64);
                assert_eq!(slot.had_gap(), i == 3);
                assert_eq!(slot.actual_channel_count(0), (i % 3) as u32);
                for channel in 0..i % 3 {
                    let samples = slot.input_channel(0, channel);
                    assert_eq!(samples[0], (i * 10 + channel) as f32);
                    assert_eq!(
                        samples[QUANTUM_SIZE - 1],
                        (i * 10 + channel) as f32 + (QUANTUM_SIZE - 1) as f32 * 0.25
                    );
                }
                assert_eq!(slot.param_block(0)[0], i as f32);
                assert_eq!(
                    slot.param_block(0)[QUANTUM_SIZE - 1],
                    i as f32 + (QUANTUM_SIZE - 1) as f32 * 0.5
                );
                assert_eq!(slot.param_block(1)[0], i as f32);
            })
        };
        assert!(popped);
    }
    assert!(!unsafe { pipe.try_read_input(|_| unreachable!()) });

    // Wrap around into the same storage: flags are cleared on push, so slot 3's gap flag must not leak through.
    for i in 0..RING_CAPACITY {
        assert!(try_push_input(&pipe, |slot| {
            slot.set_start_frame(((RING_CAPACITY + i) * QUANTUM_SIZE) as u64);
            slot.set_actual_channel_count(0, 0);
        }));
    }
    for i in 0..RING_CAPACITY {
        assert!(unsafe {
            pipe.try_read_input(|slot| {
                assert_eq!(slot.start_frame(), ((RING_CAPACITY + i) * QUANTUM_SIZE) as u64);
                assert!(!slot.had_gap());
            })
        });
    }

    assert_eq!(pipe.dropped_input_count(), 1);
    assert_eq!(pipe.underrun_count(), 0);
}

#[test]
fn output_ring_roundtrip() {
    let pipe = test_pipe();

    for i in 0..5 {
        assert!(unsafe {
            pipe.try_write_output(|slot| {
                slot.set_start_frame((i * QUANTUM_SIZE) as u64);
                slot.set_processor_active(i % 2 == 0);
                slot.set_actual_channel_count(0, 2);
                for channel in 0..2 {
                    slot.output_channel(0, channel).fill((i * 10 + channel) as f32);
                }
            })
        });
    }
    assert_eq!(pipe.output_occupancy(), 5);

    for i in 0..5 {
        assert!(try_pop_output(&pipe, |slot| {
            assert_eq!(slot.start_frame(), (i * QUANTUM_SIZE) as u64);
            assert_eq!(slot.processor_active(), i % 2 == 0);
            assert_eq!(slot.actual_channel_count(0), 2);
            for channel in 0..2 {
                assert_eq!(slot.output_channel(0, channel)[0], (i * 10 + channel) as f32);
                assert_eq!(
                    slot.output_channel(0, channel)[QUANTUM_SIZE - 1],
                    (i * 10 + channel) as f32
                );
            }
        }));
    }
    assert_eq!(pipe.underrun_count(), 0);
}

#[test]
fn priming_and_discard_down_to() {
    let pipe = test_pipe();

    unsafe { pipe.prime_outputs_with_silence(PRIME_LEVEL) };
    assert_eq!(pipe.output_occupancy(), PRIME_LEVEL);

    let popped = try_pop_output(&pipe, |slot| {
        assert_eq!(slot.start_frame(), 0);
        assert!(slot.processor_active());
        assert_eq!(slot.actual_channel_count(0), 2);
        for channel in 0..2 {
            assert!(slot.output_channel(0, channel).iter().all(|sample| *sample == 0.));
        }
    });
    assert!(popped);
    assert_eq!(pipe.output_occupancy(), PRIME_LEVEL - 1);

    pipe.discard_outputs_down_to(10);
    assert_eq!(pipe.output_occupancy(), 10);

    pipe.discard_outputs_down_to(20);
    assert_eq!(pipe.output_occupancy(), 10);

    pipe.discard_outputs_down_to(0);
    assert_eq!(pipe.output_occupancy(), 0);
    assert_eq!(pipe.underrun_count(), 0);
}

#[test]
fn state_transitions() {
    let pipe = test_pipe();

    assert_eq!(pipe.state(), WorkletPipeState::Running);
    pipe.set_state(WorkletPipeState::Failed);
    assert_eq!(pipe.state(), WorkletPipeState::Failed);
    pipe.set_state(WorkletPipeState::ShutDown);
    assert_eq!(pipe.state(), WorkletPipeState::ShutDown);
}

struct CountingClient {
    pump_runs: Cell<usize>,
}

impl AudioWorkletPipeClient for CountingClient {
    fn pump(&self) {
        self.pump_runs.set(self.pump_runs.get() + 1);
    }
}

#[test]
fn wakeup_coalescing() {
    let pipe = test_pipe();
    let client = Rc::new(CountingClient {
        pump_runs: Cell::new(0),
    });
    pipe.set_client(Some(client.clone()));

    pipe.request_wakeup();
    pipe.request_wakeup();
    pipe.request_wakeup();

    // Give any (incorrectly) duplicated wakeups a chance to run.
    run_posted_tasks();
    assert_eq!(client.pump_runs.get(), 1);

    pipe.clear_wakeup_flag();
    pipe.request_wakeup();
    run_posted_tasks();
    assert_eq!(client.pump_runs.get(), 2);

    pipe.clear_wakeup_flag();
    pipe.set_state(WorkletPipeState::ShutDown);
    pipe.request_wakeup();
    run_posted_tasks();
    assert_eq!(client.pump_runs.get(), 2);

    pipe.set_client(None);
}

struct GainClient {
    pipe: Weak<AudioWorkletPipe>,
    random: Cell<u32>,
    quanta_processed: Cell<u64>,
}

impl AudioWorkletPipeClient for GainClient {
    fn pump(&self) {
        let pipe = self.pipe.upgrade().unwrap();
        pipe.clear_wakeup_flag();
        random_sleep(&self.random);
        while unsafe {
            pipe.try_read_input(|input| {
                let pushed = pipe.try_write_output(|output| {
                    output.set_start_frame(input.start_frame());
                    output.set_processor_active(true);
                    output.set_actual_channel_count(0, input.actual_channel_count(0));
                    let gain = input.param_block(1)[0];
                    for channel in 0..input.actual_channel_count(0) as usize {
                        let source = input.input_channel(0, channel);
                        for (destination, sample) in output.output_channel(0, channel).iter_mut().zip(source) {
                            *destination = sample * gain;
                        }
                    }
                });
                assert!(pushed);
                self.quanta_processed.set(self.quanta_processed.get() + 1);
            })
        } {}
    }
}

#[derive(Default)]
struct ProducerResults {
    popped_stamps: Vec<u64>,
    sample_mismatches: u64,
    failed_output_pops: u64,
    successful_input_pushes: u64,
}

fn run_event_loop_until(done: &AtomicBool) {
    while !done.load(Ordering::SeqCst) {
        run_posted_tasks();
        thread::sleep(Duration::from_millis(5));
    }
    run_posted_tasks();
}

#[test]
fn two_thread_stress() {
    const BURST_COUNT: usize = 20;

    // Primed slots are stamped 0; real quanta start one quantum in.
    const FIRST_STAMP: u64 = QUANTUM_SIZE as u64;

    let pipe = test_pipe();
    unsafe { pipe.prime_outputs_with_silence(PRIME_LEVEL) };
    let client = Rc::new(GainClient {
        pipe: Arc::downgrade(&pipe),
        random: Cell::new(1),
        quanta_processed: Cell::new(0),
    });
    pipe.set_client(Some(client.clone()));

    let producer_done = Arc::new(AtomicBool::new(false));
    let producer = {
        let pipe = Arc::clone(&pipe);
        let producer_done = Arc::clone(&producer_done);
        thread::spawn(move || {
            let mut results = ProducerResults::default();
            results.popped_stamps.reserve(BURST_COUNT * BURST_QUANTA + PRIME_LEVEL);
            let mut next_frame = FIRST_STAMP;
            for _ in 0..BURST_COUNT {
                for _ in 0..BURST_QUANTA {
                    let popped = try_pop_output(&pipe, |output| {
                        let stamp = output.start_frame();
                        results.popped_stamps.push(stamp);

                        // Primed slots are silent; processed ones echo the value the producer filled in below.
                        for channel in 0..output.actual_channel_count(0) as usize {
                            let expected = if stamp == 0 {
                                0.
                            } else {
                                (stamp / QUANTUM_SIZE as u64 + channel as u64) as f32
                            };
                            if output.output_channel(0, channel)[0] != expected {
                                results.sample_mismatches += 1;
                            }
                        }
                    });
                    if !popped {
                        results.failed_output_pops += 1;
                    }
                    let pushed = try_push_input(&pipe, |input| {
                        input.set_start_frame(next_frame);
                        input.set_actual_channel_count(0, 2);
                        for channel in 0..2 {
                            input
                                .input_channel(0, channel)
                                .fill((next_frame / QUANTUM_SIZE as u64 + channel as u64) as f32);
                        }
                        input.param_block(0).fill(0.5);
                        input.param_block(1)[0] = 1.;
                    });
                    if pushed {
                        results.successful_input_pushes += 1;
                    }
                    next_frame += QUANTUM_SIZE as u64;
                }
                pipe.request_wakeup();
                thread::sleep(Duration::from_millis(100));
            }
            producer_done.store(true, Ordering::SeqCst);
            results
        })
    };

    run_event_loop_until(&producer_done);
    let results = producer.join().unwrap();

    let mut pending_input = 0_u64;
    while unsafe { pipe.try_read_input(|_| pending_input += 1) } {}

    assert_eq!(results.sample_mismatches, 0);
    assert_eq!(
        results.failed_output_pops + results.popped_stamps.len() as u64,
        (BURST_COUNT * BURST_QUANTA) as u64
    );
    assert_eq!(
        results.successful_input_pushes + pipe.dropped_input_count(),
        (BURST_COUNT * BURST_QUANTA) as u64
    );
    assert_eq!(
        client.quanta_processed.get() + pending_input,
        results.successful_input_pushes
    );

    // The FIFO order guarantees all primed (stamp 0) slots come out before any processed quantum, and processed
    // stamps are observed in increasing quantum order.
    let primed_popped = results.popped_stamps.iter().take_while(|stamp| **stamp == 0).count();
    assert!(primed_popped <= PRIME_LEVEL);
    let mut previous_stamp = None;
    for stamp in &results.popped_stamps[primed_popped..] {
        assert!(*stamp >= FIRST_STAMP);
        assert_eq!((stamp - FIRST_STAMP) % QUANTUM_SIZE as u64, 0);
        if let Some(previous_stamp) = previous_stamp {
            assert!(*stamp > previous_stamp);
        }
        previous_stamp = Some(*stamp);
    }

    pipe.set_client(None);
}

struct SilentClient {
    pipe: RefCell<Option<Arc<AudioWorkletPipe>>>,
    quanta_processed: Cell<u64>,
}

impl AudioWorkletPipeClient for SilentClient {
    fn pump(&self) {
        let pipe = self.pipe.borrow().clone().unwrap();
        pipe.clear_wakeup_flag();
        if pipe.state() != WorkletPipeState::Running {
            return;
        }
        while unsafe {
            pipe.try_read_input(|input| {
                let _ = pipe.try_write_output(|output| {
                    output.set_start_frame(input.start_frame());
                    output.set_processor_active(true);
                    output.set_actual_channel_count(0, 0);
                });
                self.quanta_processed.set(self.quanta_processed.get() + 1);
            })
        } {}
    }
}

#[test]
fn teardown_when_producer_drops_its_reference_mid_run() {
    let pipe = test_pipe();
    unsafe { pipe.prime_outputs_with_silence(PRIME_LEVEL) };
    let client = Rc::new(SilentClient {
        pipe: RefCell::new(Some(Arc::clone(&pipe))),
        quanta_processed: Cell::new(0),
    });
    pipe.set_client(Some(client.clone()));

    let producer_done = Arc::new(AtomicBool::new(false));
    let producer = {
        let mut producer_pipe = Some(Arc::clone(&pipe));
        let producer_done = Arc::clone(&producer_done);
        thread::spawn(move || {
            let mut next_frame = QUANTUM_SIZE as u64;
            for _ in 0..3 {
                let pipe = producer_pipe.as_ref().unwrap();
                for _ in 0..BURST_QUANTA {
                    try_pop_output(pipe, |_| {});
                    let _ = try_push_input(pipe, |input| {
                        input.set_start_frame(next_frame);
                        input.set_actual_channel_count(0, 0);
                    });
                    next_frame += QUANTUM_SIZE as u64;
                }
                pipe.request_wakeup();
                thread::sleep(Duration::from_millis(30));
            }
            let pipe = producer_pipe.take().unwrap();
            pipe.set_state(WorkletPipeState::ShutDown);
            drop(pipe);
            producer_done.store(true, Ordering::SeqCst);
        })
    };

    run_event_loop_until(&producer_done);
    producer.join().unwrap();

    assert_eq!(pipe.state(), WorkletPipeState::ShutDown);
    assert_eq!(pipe.dropped_input_count(), 0);
    assert!(client.quanta_processed.get() > 0);

    pipe.set_client(None);
    client.pipe.replace(None);
}
