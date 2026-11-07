/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::atomic::{AtomicPtr, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use ak::math;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum InsertResult {
    Success,
    EventContainedInValueCurve,
    ValueCurveOverlapsEvent,
}

#[derive(Clone, Copy)]
struct RampStart {
    set_target_event_id: usize,
    time: f64,
    value: f32,
}

#[derive(Clone)]
enum Parameterization {
    SetValue { value: f32 },
    LinearRamp { value: f32, start: Option<RampStart> },
    ExponentialRamp { value: f32, start: Option<RampStart> },
    SetTarget { target: f32, time_constant: f32 },
    SetValueCurve { values: Arc<[f32]>, duration: f64 },
    Hold { value: f32 },
}

#[derive(Clone)]
struct AutomationEvent {
    time: f64,
    parameterization: Parameterization,
    id: usize,
}

#[derive(Clone, Copy, Default)]
struct ParameterizationCache {
    event_index: Option<usize>,
    minimum_time: Option<f64>,
    maximum_time: Option<f64>,
    starting_value: f32,
}

impl ParameterizationCache {
    fn contains(&self, time: f64) -> bool {
        self.minimum_time.is_none_or(|minimum_time| time >= minimum_time)
            && self.maximum_time.is_none_or(|maximum_time| time < maximum_time)
    }
}

#[derive(Clone)]
struct TimelineState {
    generation: u32,
    fallback_value: f32,
    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    automation_events: Vec<AutomationEvent>,
    next_event_id: usize,
}

struct TimelineWriter {
    state: TimelineState,
    retired: Vec<RetiredSnapshot>,
}

struct RetiredSnapshot(*mut TimelineState);

// SAFETY: A retired snapshot is immutable and only the writer can reclaim it.
unsafe impl Send for RetiredSnapshot {}

impl Drop for RetiredSnapshot {
    fn drop(&mut self) {
        // SAFETY: The writer drops retired snapshots only after all readers of them have finished.
        drop(unsafe { Box::from_raw(self.0) });
    }
}

struct Snapshot<'a> {
    readers: &'a AtomicUsize,
    state: *const TimelineState,
}

impl std::ops::Deref for Snapshot<'_> {
    type Target = TimelineState;

    fn deref(&self) -> &TimelineState {
        // SAFETY: The snapshot stays alive for the lifetime of this reader.
        unsafe { &*self.state }
    }
}

impl Drop for Snapshot<'_> {
    fn drop(&mut self) {
        self.readers.fetch_sub(1, Ordering::SeqCst);
    }
}

// https://webaudio.github.io/web-audio-api/#dfn-automation-event
pub struct AudioParamTimeline {
    default_value: f32,
    writer: Mutex<TimelineWriter>,
    published: AtomicPtr<TimelineState>,
    readers: AtomicUsize,

    // The high bits identify the snapshot allowed to publish the low bits of [[current value]].
    current_value: AtomicU64,
}

impl Drop for AudioParamTimeline {
    fn drop(&mut self) {
        // SAFETY: No writer or reader can access the published snapshot once the timeline is dropped.
        drop(unsafe { Box::from_raw(*self.published.get_mut()) });
    }
}

impl AudioParamTimeline {
    pub fn new(default_value: f32) -> Self {
        let state = TimelineState {
            generation: 0,
            fallback_value: default_value,
            automation_events: Vec::new(),
            next_event_id: 0,
        };
        let published = AtomicPtr::new(Box::into_raw(Box::new(state.clone())));
        Self {
            default_value,
            writer: Mutex::new(TimelineWriter {
                state,
                retired: Vec::new(),
            }),
            published,
            readers: AtomicUsize::new(0),
            current_value: AtomicU64::new(u64::from(default_value.to_bits())),
        }
    }

    pub fn default_value(&self) -> f32 {
        self.default_value
    }

    pub fn current_value(&self) -> f32 {
        f32::from_bits(self.current_value.load(Ordering::Relaxed) as u32)
    }

    fn edit<R>(&self, current_value: Option<f32>, edit: impl FnOnce(&mut TimelineState, f32) -> R) -> R {
        let mut writer = self.writer.lock().unwrap();
        let generation = writer.state.generation.wrapping_add(1);
        let version = u64::from(generation) << 32;
        let previous_value = self
            .current_value
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |value| {
                Some(version | u64::from(current_value.map_or(value as u32, f32::to_bits)))
            })
            .unwrap();

        let result = edit(&mut writer.state, f32::from_bits(previous_value as u32));
        writer.state.generation = generation;
        let snapshot = Box::into_raw(Box::new(writer.state.clone()));
        let previous = self.published.swap(snapshot, Ordering::SeqCst);
        writer.retired.push(RetiredSnapshot(previous));
        // A reader entering after this check can only load the new publication.
        if self.readers.load(Ordering::SeqCst) == 0 {
            writer.retired.clear();
        }
        result
    }

    pub fn set_current_value(&self, value: f32) {
        self.edit(Some(value), |state, _| state.fallback_value = value);
    }

    fn snapshot(&self) -> Snapshot<'_> {
        // Register before loading so the writer cannot free the snapshot between the load and its first use.
        self.readers.fetch_add(1, Ordering::SeqCst);
        Snapshot {
            readers: &self.readers,
            state: self.published.load(Ordering::SeqCst),
        }
    }

    // https://webaudio.github.io/web-audio-api/#computedvalue
    pub fn value_at_time(&self, time: f64) -> f32 {
        self.snapshot().value_at_time(time, time, &mut None)
    }

    pub fn sample(&self, start_time: f64, sample_period: f64, output: &mut [f32]) {
        let state = self.snapshot();
        let mut cache = None;
        for (frame, value) in output.iter_mut().enumerate() {
            let frame_time = start_time + frame as f64 * sample_period;
            *value = state.value_at_time(frame_time + sample_period / 2., frame_time, &mut cache);
        }
        let previous_value = self.current_value.load(Ordering::SeqCst);
        if (previous_value >> 32) as u32 == state.generation {
            let next_value = (previous_value & !u64::from(u32::MAX)) | u64::from(output[0].to_bits());
            let _ = self
                .current_value
                .compare_exchange(previous_value, next_value, Ordering::SeqCst, Ordering::SeqCst);
        }
    }

    // All event times passed into these methods must already be clamped to the context's current time by the caller.
    pub fn set_value_at_time(&self, value: f32, start_time: f64) -> InsertResult {
        self.edit(None, |state, _| {
            state.insert_event(AutomationEvent {
                time: start_time,
                parameterization: Parameterization::SetValue { value },
                id: 0,
            })
        })
    }

    pub fn linear_ramp_to_value_at_time(
        &self,
        value: f32,
        end_time: f64,
        current_time: f64,
        current_value: f32,
    ) -> InsertResult {
        self.edit(None, |state, _| {
            // If there is no event preceding this event, the linear ramp behaves as if setValueAtTime(value, currentTime) were
            // called, where value is the current value of the attribute.
            let event_index = state.first_event_index_after(end_time);
            let ramp_start = state.ramp_start_for_insertion_index(state.fallback_value, event_index, current_time);
            if event_index == 0 {
                let result = state.insert_event(AutomationEvent {
                    time: current_time,
                    parameterization: Parameterization::SetValue { value: current_value },
                    id: 0,
                });
                assert_eq!(result, InsertResult::Success);
            }

            state.insert_event(AutomationEvent {
                time: end_time,
                parameterization: Parameterization::LinearRamp {
                    value,
                    start: ramp_start,
                },
                id: 0,
            })
        })
    }

    pub fn exponential_ramp_to_value_at_time(
        &self,
        value: f32,
        end_time: f64,
        current_time: f64,
        current_value: f32,
    ) -> InsertResult {
        self.edit(None, |state, _| {
            // If there is no event preceding this event, the exponential ramp behaves as if setValueAtTime(value, currentTime)
            // were called, where value is the current value of the attribute.
            let event_index = state.first_event_index_after(end_time);
            let ramp_start = state.ramp_start_for_insertion_index(state.fallback_value, event_index, current_time);
            if event_index == 0 {
                let result = state.insert_event(AutomationEvent {
                    time: current_time,
                    parameterization: Parameterization::SetValue { value: current_value },
                    id: 0,
                });
                assert_eq!(result, InsertResult::Success);
            }

            state.insert_event(AutomationEvent {
                time: end_time,
                parameterization: Parameterization::ExponentialRamp {
                    value,
                    start: ramp_start,
                },
                id: 0,
            })
        })
    }

    pub fn set_target_at_time(&self, target: f32, start_time: f64, time_constant: f32) -> InsertResult {
        self.edit(None, |state, _| {
            state.insert_event(AutomationEvent {
                time: start_time,
                parameterization: Parameterization::SetTarget { target, time_constant },
                id: 0,
            })
        })
    }

    pub fn set_value_curve_at_time(&self, values: Vec<f32>, start_time: f64, duration: f64) -> InsertResult {
        self.edit(None, |state, _| {
            // If there are any events with a time strictly greater than startTime but strictly less than startTime + duration,
            // a NotSupportedError exception MUST be thrown.
            for event in &state.automation_events {
                if event.time > start_time && event.time < start_time + duration {
                    return InsertResult::ValueCurveOverlapsEvent;
                }
            }

            state.insert_event(AutomationEvent {
                time: start_time,
                parameterization: Parameterization::SetValueCurve {
                    values: values.into(),
                    duration,
                },
                id: 0,
            })
        })
    }

    pub fn cancel_scheduled_values(&self, cancel_time: f64) {
        self.edit(None, |state, previous_value| {
            // Cancel all scheduled parameter changes with times greater than or equal to cancelTime.
            let mut first_event_to_remove = state
                .automation_events
                .partition_point(|event| event.time < cancel_time);

            // Any active automations whose event time is less than cancelTime are also cancelled. A SetTarget remains active
            // until the next event, while a SetValueCurve is active through the end of its duration.
            if first_event_to_remove > 0 {
                let previous_event = &state.automation_events[first_event_to_remove - 1];
                let is_active_automation = match &previous_event.parameterization {
                    Parameterization::SetTarget { .. } => true,
                    Parameterization::SetValueCurve { duration, .. } => cancel_time <= previous_event.time + duration,
                    _ => false,
                };
                if is_active_automation {
                    first_event_to_remove -= 1;
                }
            }

            if first_event_to_remove < state.automation_events.len() {
                state.automation_events.truncate(first_event_to_remove);
                if state.automation_events.is_empty() {
                    state.fallback_value = previous_value;
                }
            }
        });
    }

    pub fn cancel_and_hold_at_time(&self, cancel_time: f64) {
        self.edit(None, |state, _| {
            let value_to_hold = state.value_at_time(cancel_time, cancel_time, &mut None);
            let event_index = state.first_event_index_after(cancel_time);

            // If the event immediately after cancelTime is a ramp, rewrite it to end at cancelTime with the value from the
            // original timeline. This takes precedence over holding the value of the event active at cancelTime.
            let mut rewrote_ramp = false;
            if event_index < state.automation_events.len() {
                let next_event = &mut state.automation_events[event_index];
                rewrote_ramp = match &mut next_event.parameterization {
                    Parameterization::LinearRamp { value, .. } | Parameterization::ExponentialRamp { value, .. } => {
                        next_event.time = cancel_time;
                        *value = value_to_hold;
                        true
                    }
                    _ => false,
                };
            }

            // Otherwise, decide which value (if any) to hold from cancelTime onward, based on the event active at cancelTime.
            let mut hold_value = None;
            let mut drop_active_curve = false;
            if !rewrote_ramp && event_index > 0 {
                let active_event = &state.automation_events[event_index - 1];
                hold_value = match &active_event.parameterization {
                    Parameterization::SetTarget { .. } => Some(value_to_hold),
                    Parameterization::SetValueCurve { duration, .. } => {
                        // Cancelling exactly at a curve's start truncates it to a zero-duration curve, which produces no
                        // output, so the value reverts to the value before the curve rather than the curve's first element.
                        if active_event.time == cancel_time {
                            drop_active_curve = true;
                            Some(if event_index >= 2 {
                                state.event_value_at_time(state.fallback_value, event_index - 2, cancel_time)
                            } else {
                                state.fallback_value
                            })
                        } else if cancel_time <= active_event.time + duration {
                            Some(value_to_hold)
                        } else {
                            None
                        }
                    }
                    _ => None,
                };
            }

            // Curve removal and hold insertion mutate the event list, so they run after the visit rather than inside it.
            if drop_active_curve {
                state.automation_events.remove(event_index - 1);
            }
            if let Some(hold_value) = hold_value {
                assert_eq!(
                    state.insert_event(AutomationEvent {
                        time: cancel_time,
                        parameterization: Parameterization::Hold { value: hold_value },
                        id: 0
                    }),
                    InsertResult::Success
                );
            }

            // Remove all events with times greater than cancelTime. The rewritten ramp or inserted hold remains at cancelTime.
            let first_event_to_remove = state.first_event_index_after(cancel_time);
            if first_event_to_remove < state.automation_events.len() {
                state.automation_events.truncate(first_event_to_remove);
            }
        });
    }
}

impl TimelineState {
    fn value_at_time(&self, selection_time: f64, time: f64, cache: &mut Option<ParameterizationCache>) -> f32 {
        // NB: The governing automation event is selected using selection_time, which render quantum sampling sets half a
        //     frame ahead of the evaluation time so events take effect at the nearest sample frame of their scheduled time
        //     rather than the next frame when floating-point rounding pushes them past the exact frame time.
        let fallback_value = self.fallback_value;
        if cache.is_none_or(|cache| !cache.contains(selection_time)) {
            *cache = Some(self.parameterization_for_time(fallback_value, selection_time));
        }
        let cache = cache.unwrap();
        let Some(event_index) = cache.event_index else {
            return cache.starting_value;
        };

        let event = &self.automation_events[event_index];
        match &event.parameterization {
            Parameterization::SetValue { value } | Parameterization::Hold { value } => *value,
            Parameterization::LinearRamp { value, .. } => {
                // NB: When a ramp ends between two sample frames, selection can consider the ramp completed while the
                //     evaluation time still lies just before its end time. The cached minimum time is then the ramp's own
                //     end time, so interpolating would divide by zero; the ramp's end value applies instead.
                let minimum_time = cache.minimum_time.unwrap();
                if time >= event.time || event.time <= minimum_time {
                    return *value;
                }

                let progress = ((time - minimum_time) / (event.time - minimum_time)) as f32;
                cache.starting_value + (value - cache.starting_value) * progress
            }
            Parameterization::ExponentialRamp { value, .. } => {
                let minimum_time = cache.minimum_time.unwrap();
                if time >= event.time || event.time <= minimum_time {
                    return *value;
                }

                if cache.starting_value == 0.
                    || (cache.starting_value < 0. && *value > 0.)
                    || (cache.starting_value > 0. && *value < 0.)
                {
                    return cache.starting_value;
                }

                let progress = ((time - minimum_time) / (event.time - minimum_time)) as f32;
                cache.starting_value * math::pow(value / cache.starting_value, progress)
            }
            Parameterization::SetTarget { target, time_constant } => {
                if *time_constant == 0. {
                    return *target;
                }
                target
                    + (cache.starting_value - target)
                        * math::exp(-(time - event.time) / f64::from(*time_constant)) as f32
            }
            Parameterization::SetValueCurve { .. } => self.event_value_at_time(fallback_value, event_index, time),
        }
    }

    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    fn first_event_index_after(&self, time: f64) -> usize {
        self.automation_events.partition_point(|event| event.time <= time)
    }

    fn event_value_at_time(&self, default_value: f32, event_index: usize, time: f64) -> f32 {
        let mut first_event_index = event_index;
        while first_event_index > 0
            && matches!(
                self.automation_events[first_event_index].parameterization,
                Parameterization::SetTarget { .. }
            )
        {
            first_event_index -= 1;
        }

        let mut value = default_value;
        for current_event_index in first_event_index..=event_index {
            let event = &self.automation_events[current_event_index];
            let evaluation_time = if current_event_index == event_index {
                time
            } else {
                self.automation_events[current_event_index + 1].time
            };
            value = match &event.parameterization {
                Parameterization::SetValue { value }
                | Parameterization::LinearRamp { value, .. }
                | Parameterization::ExponentialRamp { value, .. }
                | Parameterization::Hold { value } => *value,
                Parameterization::SetTarget { target, time_constant } => {
                    if *time_constant == 0. {
                        *target
                    } else {
                        target
                            + (value - target)
                                * math::exp(-(evaluation_time - event.time) / f64::from(*time_constant)) as f32
                    }
                }
                Parameterization::SetValueCurve { values, duration } => {
                    if evaluation_time >= event.time + duration {
                        *values.last().unwrap()
                    } else {
                        let curve_position = (values.len() - 1) as f64 * (evaluation_time - event.time) / duration;

                        // NB: Floating-point rounding can push curve_position to size() - 1 even though evaluation_time is
                        //     still strictly less than the curve's end time, so value_index is clamped to keep value_index + 1 in bounds.
                        let value_index = (curve_position.floor() as usize).min(values.len() - 2);
                        let interpolation_factor = (curve_position - value_index as f64) as f32;
                        values[value_index] + (values[value_index + 1] - values[value_index]) * interpolation_factor
                    }
                }
            };
        }
        value
    }

    // A ramp after an already-started SetTarget begins at currentTime using the target's value at that time.
    fn ramp_start_for_insertion_index(
        &self,
        default_value: f32,
        event_index: usize,
        current_time: f64,
    ) -> Option<RampStart> {
        if event_index == 0 {
            return None;
        }

        let previous_event = &self.automation_events[event_index - 1];
        if previous_event.time >= current_time
            || !matches!(previous_event.parameterization, Parameterization::SetTarget { .. })
        {
            return None;
        }

        Some(RampStart {
            set_target_event_id: previous_event.id,
            time: current_time,
            value: self.event_value_at_time(default_value, event_index - 1, current_time),
        })
    }

    // https://webaudio.github.io/web-audio-api/#computedvalue
    fn parameterization_for_time(&self, default_value: f32, time: f64) -> ParameterizationCache {
        let event_index = self.first_event_index_after(time);
        if event_index == 0 {
            return ParameterizationCache {
                maximum_time: self.automation_events.first().map(|event| event.time),
                starting_value: default_value,
                ..Default::default()
            };
        }

        // A following ramp parameterization owns the interval before its event time. Other parameterizations fall through
        // to caching the preceding event below.
        if event_index < self.automation_events.len() {
            let cache = match &self.automation_events[event_index].parameterization {
                Parameterization::SetValue { .. }
                | Parameterization::SetTarget { .. }
                | Parameterization::SetValueCurve { .. }
                | Parameterization::Hold { .. } => None,
                Parameterization::LinearRamp { start, .. } | Parameterization::ExponentialRamp { start, .. } => {
                    let previous_event = &self.automation_events[event_index - 1];
                    let (minimum_time, starting_value);
                    if let Some(start) = start
                        && start.set_target_event_id == previous_event.id
                    {
                        minimum_time = start.time;
                        starting_value = start.value;
                    } else {
                        minimum_time = match &previous_event.parameterization {
                            Parameterization::SetValueCurve { duration, .. } => previous_event.time + duration,
                            _ => previous_event.time,
                        };
                        starting_value = self.event_value_at_time(default_value, event_index - 1, minimum_time);
                    }

                    // NB: A ramp rewritten by cancelAndHoldAtTime() can end before a preceding value curve's original end.
                    //     In that case, the curve owns the interval before the ramp endpoint.
                    if time < minimum_time || minimum_time >= self.automation_events[event_index].time {
                        None
                    } else {
                        Some(ParameterizationCache {
                            event_index: Some(event_index),
                            minimum_time: Some(minimum_time),
                            maximum_time: Some(self.automation_events[event_index].time),
                            starting_value,
                        })
                    }
                }
            };
            if let Some(cache) = cache {
                return cache;
            }
        }

        let selected_event_index = event_index - 1;
        let selected_event = &self.automation_events[selected_event_index];
        let mut maximum_time = None;
        if event_index < self.automation_events.len() {
            let next_event = &self.automation_events[event_index];
            maximum_time = Some(match &next_event.parameterization {
                Parameterization::LinearRamp { start, .. } | Parameterization::ExponentialRamp { start, .. } => {
                    if let Some(start) = start
                        && start.set_target_event_id == selected_event.id
                    {
                        start.time
                    } else {
                        next_event.time
                    }
                }
                _ => next_event.time,
            });
        }
        ParameterizationCache {
            event_index: Some(selected_event_index),
            minimum_time: Some(selected_event.time),
            maximum_time,
            starting_value: self.event_value_at_time(default_value, selected_event_index, selected_event.time),
        }
    }

    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    fn insert_event(&mut self, mut event: AutomationEvent) -> InsertResult {
        let insertion_index = self.first_event_index_after(event.time);

        // If any automation method is called at a time contained in a SetValueCurve event, a NotSupportedError exception
        // MUST be thrown.
        if insertion_index > 0 {
            let previous_event = &self.automation_events[insertion_index - 1];
            let is_contained_in_curve = match &previous_event.parameterization {
                Parameterization::SetValueCurve { duration, .. } => event.time < previous_event.time + duration,
                _ => false,
            };

            // NB: A hold event ends an active value curve without changing how the curve is sampled before the hold time.
            if is_contained_in_curve && !matches!(event.parameterization, Parameterization::Hold { .. }) {
                return InsertResult::EventContainedInValueCurve;
            }
        }

        // If an event is added at a time where there are already events, it is placed after them but before later events.
        event.id = self.next_event_id;
        self.next_event_id += 1;
        self.automation_events.insert(insertion_index, event);
        InsertResult::Success
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn render_reads_a_complete_snapshot_while_the_writer_is_busy() {
        let timeline = AudioParamTimeline::new(1.);
        assert_eq!(timeline.set_value_at_time(2., 0.), InsertResult::Success);
        std::thread::scope(|scope| {
            let (send, receive) = std::sync::mpsc::channel();
            let writer = timeline.writer.lock().unwrap();
            let timeline = &timeline;
            scope.spawn(move || {
                let mut output = [0.; 128];
                timeline.sample(0., 1. / 48000., &mut output);
                send.send(output).unwrap();
            });
            let result = receive.recv_timeout(std::time::Duration::from_secs(5));
            drop(writer);
            assert_eq!(result.unwrap(), [2.; 128]);
        });

        let old_snapshot = timeline.snapshot();
        assert_eq!(timeline.set_value_at_time(3., 0.), InsertResult::Success);
        assert_eq!(old_snapshot.value_at_time(0., 0., &mut None), 2.);
        assert_eq!(timeline.value_at_time(0.), 3.);
    }
}
