/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::UnsafeCell;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU64, AtomicUsize, Ordering};

use crate::web_audio::control_thread::{ControlThread, ControlThreadCell};

pub trait AudioWorkletPipeClient {
    fn pump(&self);
}

pub struct Config {
    pub quantum_size: usize,
    pub input_count: usize,
    pub output_count: usize,

    // Per input: the largest channel count a slot can carry.
    pub input_channel_capacity: Vec<u32>,

    // Per output: the largest channel count a slot can carry.
    pub output_channel_capacity: Vec<u32>,
    pub output_channel_count_matches_input: bool,
    pub param_count: usize,

    // Per parameter: a-rate blocks carry quantum_size samples, k-rate blocks one.
    pub param_is_a_rate: Vec<bool>,

    // Slots per ring; must be a power of two.
    pub ring_capacity: usize,

    // Silent output quanta pushed before rendering starts; at most ring_capacity.
    pub prime_level: usize,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum WorkletPipeState {
    Running,
    Failed,
    ShutDown,
}

#[repr(align(64))]
#[derive(Default)]
struct Counter(AtomicUsize);

#[derive(Default)]
struct Ring {
    // Producer-owned: the next slot to write.
    tail: Counter,

    // Consumer-owned: the next slot to read.
    head: Counter,
}

// A lock-free bridge between the realtime render node and the main-thread processor. Two SPSC rings share a
// preallocated arena; underruns render silence and a full input ring drops quanta. Each ring uses acquire/release
// head-tail discipline, and the slot stride is configured at run time.
//
// Per-slot layout; strides are rounded up to 64 bytes so slots start cache-line-aligned relative to the arena:
//
//   Input slot                                            Output slot
//     u64 start_frame                                       u64 start_frame
//     u32 flags (bit 0: gap before this quantum)            u32 flags (bit 0: processor active; bit 1: reserved
//     u32 actual_channel_count[input_count]                     for a per-quantum processing-error signal)
//     f32 samples[input_channel_capacity[0]][quantum_size]  u32 actual_channel_count[output_count]
//       ... one sample block per input ...                  f32 samples[output_channel_capacity[0]][quantum_size]
//     f32 param_block[param_count]: quantum_size samples      ... one sample block per output ...
//         each for a-rate parameters, 1 for k-rate
//
// Slots are accessed in place through the writer/reader views below, which are only valid inside the callback they
// are passed to. A slot's unwritten regions keep stale data from the previous trip around the ring, so a writer
// must set the channel counts it wants a reader to trust; the flags are cleared on every push.
pub struct AudioWorkletPipe {
    pub config: Config,
    ring_index_mask: usize,
    input_slot_stride: usize,
    output_slot_stride: usize,
    output_region_offset: usize,
    input_samples_offset: Vec<usize>,
    output_samples_offset: Vec<usize>,
    param_block_offset: Vec<usize>,
    param_block_length: Vec<usize>,
    arena: Box<[UnsafeCell<u64>]>,
    input_ring: Ring,
    output_ring: Ring,
    control_thread: ControlThread,
    client: ControlThreadCell<Option<Rc<dyn AudioWorkletPipeClient>>>,
    pump_scheduled: AtomicBool,
    state: AtomicU8,
    underrun_count: AtomicU64,
    dropped_input_count: AtomicU64,
}

// SAFETY: Each ring transfers its disjoint arena slots between one producer and one consumer with acquire/release counters.
unsafe impl Send for AudioWorkletPipe {}
unsafe impl Sync for AudioWorkletPipe {}
const SLOT_FLAGS_OFFSET: usize = 8;
const SLOT_CHANNEL_COUNTS_OFFSET: usize = 12;
const SLOT_ALIGNMENT: usize = 64;

impl AudioWorkletPipe {
    pub fn new(config: Config) -> Arc<Self> {
        assert!(config.quantum_size > 0);
        assert_eq!(config.input_channel_capacity.len(), config.input_count);
        assert_eq!(config.output_channel_capacity.len(), config.output_count);
        assert_eq!(config.param_is_a_rate.len(), config.param_count);
        assert!(config.ring_capacity.is_power_of_two());
        assert!(config.prime_level <= config.ring_capacity);
        let channel_bytes = config.quantum_size * size_of::<f32>();
        let mut offset = SLOT_CHANNEL_COUNTS_OFFSET + config.input_count * size_of::<u32>();
        let mut input_samples_offset = Vec::with_capacity(config.input_count);
        for input in 0..config.input_count {
            input_samples_offset.push(offset);
            offset += config.input_channel_capacity[input] as usize * channel_bytes;
        }
        let mut param_block_offset = Vec::with_capacity(config.param_count);
        let mut param_block_length = Vec::with_capacity(config.param_count);
        for param in 0..config.param_count {
            param_block_offset.push(offset);
            let block_length = if config.param_is_a_rate[param] {
                config.quantum_size
            } else {
                1
            };
            param_block_length.push(block_length);
            offset += block_length * size_of::<f32>();
        }
        let input_slot_stride = (offset + SLOT_ALIGNMENT - 1) & !(SLOT_ALIGNMENT - 1);
        offset = SLOT_CHANNEL_COUNTS_OFFSET + config.output_count * size_of::<u32>();
        let mut output_samples_offset = Vec::with_capacity(config.output_count);
        for output in 0..config.output_count {
            output_samples_offset.push(offset);
            offset += config.output_channel_capacity[output] as usize * channel_bytes;
        }
        let output_slot_stride = (offset + SLOT_ALIGNMENT - 1) & !(SLOT_ALIGNMENT - 1);
        let output_region_offset = config.ring_capacity * input_slot_stride;
        let arena_size = output_region_offset + config.ring_capacity * output_slot_stride;
        Arc::new(Self {
            ring_index_mask: config.ring_capacity - 1,
            config,
            input_slot_stride,
            output_slot_stride,
            output_region_offset,
            input_samples_offset,
            output_samples_offset,
            param_block_offset,
            param_block_length,
            arena: (0..arena_size / size_of::<u64>()).map(|_| UnsafeCell::new(0)).collect(),
            input_ring: Ring::default(),
            output_ring: Ring::default(),
            control_thread: ControlThread::current(),
            client: ControlThreadCell::new(None),
            pump_scheduled: AtomicBool::new(false),
            state: AtomicU8::new(WorkletPipeState::Running as u8),
            underrun_count: AtomicU64::new(0),
            dropped_input_count: AtomicU64::new(0),
        })
    }

    // Accessed only on the control thread, including by deferred wakeups.
    pub fn set_client(&self, client: Option<Rc<dyn AudioWorkletPipeClient>>) {
        self.client.replace(client);
    }

    fn input_slot(&self, counter: usize) -> *mut u8 {
        unsafe {
            self.arena
                .as_ptr()
                .cast::<u8>()
                .cast_mut()
                .add((counter & self.ring_index_mask) * self.input_slot_stride)
        }
    }

    fn output_slot(&self, counter: usize) -> *mut u8 {
        unsafe {
            self.arena
                .as_ptr()
                .cast::<u8>()
                .cast_mut()
                .add(self.output_region_offset + (counter & self.ring_index_mask) * self.output_slot_stride)
        }
    }

    // Fills the next input slot in place. Returns false, and counts a dropped quantum, when the ring is full; the
    // caller should then mark the next successful push with set_had_gap() so the processor can tell that quanta
    // went missing.
    pub(crate) unsafe fn try_push_input(&self, fill: impl FnOnce(*mut u8)) -> bool {
        let tail = self.input_ring.tail.0.load(Ordering::Relaxed);
        if tail.wrapping_sub(self.input_ring.head.0.load(Ordering::Acquire)) == self.config.ring_capacity {
            self.dropped_input_count.fetch_add(1, Ordering::Relaxed);
            return false;
        }
        let slot = self.input_slot(tail);
        unsafe {
            *slot.add(SLOT_FLAGS_OFFSET).cast::<u32>() = 0;
        }
        fill(slot);
        self.input_ring.tail.0.store(tail.wrapping_add(1), Ordering::Release);
        true
    }

    pub(crate) unsafe fn try_pop_input(&self, read: impl FnOnce(*mut u8)) -> bool {
        let head = self.input_ring.head.0.load(Ordering::Relaxed);
        if head == self.input_ring.tail.0.load(Ordering::Acquire) {
            return false;
        }
        read(self.input_slot(head));
        self.input_ring.head.0.store(head.wrapping_add(1), Ordering::Release);
        true
    }

    pub(crate) unsafe fn try_push_output(&self, fill: impl FnOnce(*mut u8)) -> bool {
        let tail = self.output_ring.tail.0.load(Ordering::Relaxed);
        if tail.wrapping_sub(self.output_ring.head.0.load(Ordering::Acquire)) == self.config.ring_capacity {
            return false;
        }
        let slot = self.output_slot(tail);
        unsafe {
            *slot.add(SLOT_FLAGS_OFFSET).cast::<u32>() = 0;
        }
        fill(slot);
        self.output_ring.tail.0.store(tail.wrapping_add(1), Ordering::Release);
        true
    }

    // Reads the oldest processed quantum. Returns false, and counts an underrun, when none is ready; the caller
    // then outputs silence, never blocking the audio callback.
    pub(crate) unsafe fn try_pop_output(&self, read: impl FnOnce(*mut u8)) -> bool {
        let head = self.output_ring.head.0.load(Ordering::Relaxed);
        if head == self.output_ring.tail.0.load(Ordering::Acquire) {
            self.underrun_count.fetch_add(1, Ordering::Relaxed);
            return false;
        }
        read(self.output_slot(head));
        self.output_ring.head.0.store(head.wrapping_add(1), Ordering::Release);
        true
    }

    pub fn output_occupancy(&self) -> usize {
        self.output_ring
            .tail
            .0
            .load(Ordering::Acquire)
            .wrapping_sub(self.output_ring.head.0.load(Ordering::Relaxed))
    }

    // Drops the oldest processed quanta until at most target_occupancy remain. After a main-thread stall the pump
    // catches up and refills the output ring past the prime level; clamping back down keeps the pipe's added
    // latency from ratcheting up permanently.
    pub fn discard_outputs_down_to(&self, target_occupancy: usize) {
        // A consumer-side pop-without-read: advancing the head releases the discarded slots back to the producer. New
        // pushes can only move the tail further along, so the snapshot below never overshoots.
        let head = self.output_ring.head.0.load(Ordering::Relaxed);
        let tail = self.output_ring.tail.0.load(Ordering::Acquire);
        if tail.wrapping_sub(head) <= target_occupancy {
            return;
        }
        self.output_ring
            .head
            .0
            .store(tail.wrapping_sub(target_occupancy), Ordering::Release);
    }

    /// Pushes `count` silent, processor-active output quanta. Called on the control thread before the pipe is handed
    /// to the render node, so the audio thread has a burst worth of lookahead before the first processed quantum.
    ///
    /// # Safety
    /// The caller must be the sole producer of the output ring.
    pub unsafe fn prime_outputs_with_silence(&self, count: usize) {
        for _i in 0..count {
            let pushed = unsafe {
                self.try_push_output(|slot| {
                    Self::set_start_frame(slot, 0);
                    Self::set_flag(slot, true);
                    for output in 0..self.config.output_count {
                        let channel_count = if self.config.output_channel_count_matches_input {
                            1
                        } else {
                            self.config.output_channel_capacity[output]
                        };
                        *self.channel_count(slot, true, output) = channel_count;
                        for channel in 0..channel_count {
                            let (samples, length) = self.channel(slot, true, output, channel as usize);
                            ak::slice_from_raw_parts_mut(samples, length).fill(0.);
                        }
                    }
                })
            };
            assert!(pushed);
        }
    }

    // Schedules the pump on the main thread's event loop. Coalesced: only the first request after
    // clear_wakeup_flag() posts an event, so a device burst of several dozen quanta causes one pump run.
    pub fn request_wakeup(self: &Arc<Self>) {
        // Coalesce a device burst into one main-thread pump instead of posting once per quantum.
        if self.pump_scheduled.swap(true, Ordering::AcqRel) {
            return;
        }
        let pipe = Arc::clone(self);
        self.control_thread.post(move || {
            if pipe.state() == WorkletPipeState::ShutDown {
                return;
            }
            if let Some(client) = pipe.client.get() {
                client.pump();
            }
        });
    }

    // The pump calls this before draining the input ring, so a burst that lands while the pump is running schedules
    // another run instead of being lost.
    pub fn clear_wakeup_flag(&self) {
        // A read-modify-write rather than a plain store: it synchronizes with the audio thread's exchange in
        // request_wakeup(), so whenever that exchange elides a wakeup (returns true), everything the audio thread
        // pushed beforehand is visible to this pump run's drain.
        self.pump_scheduled.swap(false, Ordering::AcqRel);
    }

    pub fn state(&self) -> WorkletPipeState {
        match self.state.load(Ordering::Acquire) {
            0 => WorkletPipeState::Running,
            1 => WorkletPipeState::Failed,
            2 => WorkletPipeState::ShutDown,
            _ => unreachable!(),
        }
    }

    pub fn set_state(&self, state: WorkletPipeState) {
        self.state.store(state as u8, Ordering::Release);
    }

    pub fn underrun_count(&self) -> u64 {
        self.underrun_count.load(Ordering::Relaxed)
    }

    pub fn dropped_input_count(&self) -> u64 {
        self.dropped_input_count.load(Ordering::Relaxed)
    }

    /// # Safety
    /// The caller must be the only consumer of the input ring.
    pub unsafe fn try_read_input(&self, read: impl FnOnce(&InputSlotReader<'_>)) -> bool {
        unsafe { self.try_pop_input(|slot| read(&InputSlotReader { pipe: self, slot })) }
    }

    /// # Safety
    /// The caller must be the only producer of the output ring.
    pub unsafe fn try_write_output(&self, fill: impl FnOnce(&mut OutputSlotWriter<'_>)) -> bool {
        unsafe { self.try_push_output(|slot| fill(&mut OutputSlotWriter { pipe: self, slot })) }
    }

    pub(crate) unsafe fn start_frame(slot: *const u8) -> u64 {
        unsafe { *slot.cast::<u64>() }
    }

    pub(crate) unsafe fn set_start_frame(slot: *mut u8, frame: u64) {
        unsafe {
            *slot.cast::<u64>() = frame;
        }
    }

    pub(crate) unsafe fn set_flag(slot: *mut u8, enabled: bool) {
        let flags = unsafe { &mut *slot.add(SLOT_FLAGS_OFFSET).cast::<u32>() };
        if enabled {
            *flags |= 1;
        } else {
            *flags &= !1;
        }
    }

    pub(crate) unsafe fn flag(slot: *const u8) -> bool {
        unsafe { *slot.add(SLOT_FLAGS_OFFSET).cast::<u32>() & 1 != 0 }
    }

    pub(crate) unsafe fn channel_count(&self, slot: *mut u8, output: bool, index: usize) -> *mut u32 {
        assert!(
            index
                < if output {
                    self.config.output_count
                } else {
                    self.config.input_count
                }
        );
        unsafe { slot.add(SLOT_CHANNEL_COUNTS_OFFSET + index * size_of::<u32>()).cast() }
    }

    pub(crate) unsafe fn channel(
        &self,
        slot: *mut u8,
        output: bool,
        index: usize,
        channel: usize,
    ) -> (*mut f32, usize) {
        let (capacity, offsets) = if output {
            (&self.config.output_channel_capacity, &self.output_samples_offset)
        } else {
            (&self.config.input_channel_capacity, &self.input_samples_offset)
        };
        assert!(index < capacity.len());
        assert!(channel < capacity[index] as usize);
        (
            unsafe {
                slot.add(offsets[index])
                    .cast::<f32>()
                    .add(channel * self.config.quantum_size)
            },
            self.config.quantum_size,
        )
    }

    pub(crate) unsafe fn param_block(&self, slot: *mut u8, param: usize) -> (*mut f32, usize) {
        assert!(param < self.config.param_count);
        (
            unsafe { slot.add(self.param_block_offset[param]).cast() },
            self.param_block_length[param],
        )
    }
}

pub struct RingSizing {
    pub ring_capacity: usize,
    pub prime_level: usize,
}

// The renderer produces a device buffer's worth of quanta in one burst, so lookahead must cover a full burst plus
// scheduling margin. At the 100 ms, 48 kHz defaults this is 38 quanta, a prime level of 48, and capacity 64.
pub fn ring_sizing_for_device_latency(device_latency_ms: u32, sample_rate: f32, quantum_size: usize) -> RingSizing {
    assert!(sample_rate > 0.);
    assert!(quantum_size > 0);
    let burst_quanta =
        (f64::from(device_latency_ms) / 1000. * f64::from(sample_rate) / quantum_size as f64).ceil() as usize;
    let prime = (burst_quanta * 5).div_ceil(4);
    let mut capacity = 1;
    while capacity < prime + (burst_quanta / 4).max(1) {
        capacity *= 2;
    }
    RingSizing {
        ring_capacity: capacity,
        prime_level: prime,
    }
}

pub struct InputSlotReader<'a> {
    pipe: &'a AudioWorkletPipe,
    slot: *mut u8,
}

impl InputSlotReader<'_> {
    pub fn start_frame(&self) -> u64 {
        unsafe { AudioWorkletPipe::start_frame(self.slot) }
    }

    pub fn had_gap(&self) -> bool {
        unsafe { AudioWorkletPipe::flag(self.slot) }
    }

    pub fn actual_channel_count(&self, input: usize) -> u32 {
        unsafe { *self.pipe.channel_count(self.slot, false, input) }
    }

    pub fn input_channel(&self, input: usize, channel: usize) -> &[f32] {
        let (samples, length) = unsafe { self.pipe.channel(self.slot, false, input, channel) };
        unsafe { ak::slice_from_raw_parts(samples, length) }
    }

    pub fn param_block(&self, param: usize) -> &[f32] {
        let (samples, length) = unsafe { self.pipe.param_block(self.slot, param) };
        unsafe { ak::slice_from_raw_parts(samples, length) }
    }
}

pub struct OutputSlotWriter<'a> {
    pipe: &'a AudioWorkletPipe,
    slot: *mut u8,
}

impl OutputSlotWriter<'_> {
    pub fn set_start_frame(&mut self, frame: u64) {
        unsafe { AudioWorkletPipe::set_start_frame(self.slot, frame) };
    }

    pub fn set_processor_active(&mut self, active: bool) {
        unsafe { AudioWorkletPipe::set_flag(self.slot, active) };
    }

    pub fn set_actual_channel_count(&mut self, output: usize, channel_count: u32) {
        unsafe { *self.pipe.channel_count(self.slot, true, output) = channel_count };
    }

    pub fn output_channel(&mut self, output: usize, channel: usize) -> &mut [f32] {
        let (samples, length) = unsafe { self.pipe.channel(self.slot, true, output, channel) };
        unsafe { ak::slice_from_raw_parts_mut(samples, length) }
    }
}
