/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, Trace, Vm};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::CppEvent;
use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Ref, cell_from_abi, cell_into_abi};
use crate::event_timing::event_counts::EventCounts;
use crate::event_timing::performance_event_timing::PerformanceEventTiming;
use crate::performance_timeline::performance_entry::AnyPerformanceEntry;
use crate::performance_timeline::performance_timeline::PerformanceTimeline;
use crate::ui_events::input_event::CppInputEvent;
use crate::ui_events::keyboard_event::CppKeyboardEvent;
use crate::ui_events::mouse_event::is_mouse_event;
use crate::ui_events::pointer_event::{CppPointerEvent, MOUSE_POINTER_ID};

unsafe extern "C" {
    fn web_event_timing_request_frame(window: *mut c_void);
    fn web_high_resolution_time_current_high_resolution_time(global_object: *mut c_void) -> f64;
}

/// https://w3c.github.io/event-timing/#sec-modifications-HTML
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WindowEventTimingState {
    // entries to be queued, a list that stores PerformanceEventTiming objects, which will initially be empty.
    entries_to_be_queued: GcRefCell<Vec<Ref<PerformanceEventTiming>>>,
    // has dispatched input event, a boolean which is initially set to false.
    has_dispatched_input_event: Cell<bool>,
    // user interaction value, an integer which is initially set to a random integer between 100 and 10000.
    user_interaction_value: Cell<u64>,
    // pending key downs, a map of integers to PerformanceEventTimings which is initially empty.
    pending_key_downs: GcRefCell<Vec<(u32, Ref<PerformanceEventTiming>)>>,
    // pointer interaction value map, a map of integers which is initially empty.
    #[gc(untraced)]
    pointer_interaction_value_map: RefCell<Vec<(i32, u64)>>,
    // pending pointer downs, a map of integers to PerformanceEventTimings which is initially empty.
    pending_pointer_downs: GcRefCell<Vec<(i32, Ref<PerformanceEventTiming>)>>,
    // is contextmenu triggered, a boolean which is initially set to false.
    is_contextmenu_triggered: Cell<bool>,
    // eventCounts, a map with entries of the form type → numEvents.
    // NB: This is created when it is first asked for.
    event_counts: Cell<Option<Ref<EventCounts>>>,
    // interactionCount, an integer which counts the total number of distinct user interactions, for which there was a
    // unique interactionId computed via computing interactionId.
    interaction_count: Cell<u64>,
}

static WINDOW_EVENT_TIMING_STATE_CLASS: EmbedderClass<WindowEventTimingState> =
    EmbedderClass::new("WindowEventTimingState");

impl EmbedderValue for WindowEventTimingState {
    const CLASS: &'static EmbedderClass<Self> = &WINDOW_EVENT_TIMING_STATE_CLASS;
}

pub(crate) type WindowEventTimingStateRef = Gc<EmbedderCell<WindowEventTimingState>>;

impl WindowEventTimingState {
    pub(crate) fn create(vm: &Vm) -> WindowEventTimingStateRef {
        // NB: The user interaction value is initially set to a random integer between 100 and 10000.
        let mut random_bytes = [0u8; 8];
        libcrypto_rust::fill_with_random(&mut random_bytes);
        let user_interaction_value = 100 + u64::from_ne_bytes(random_bytes) % 9901;

        WINDOW_EVENT_TIMING_STATE_CLASS.allocate(vm, |_| Self {
            entries_to_be_queued: GcRefCell::default(),
            has_dispatched_input_event: Cell::new(false),
            user_interaction_value: Cell::new(user_interaction_value),
            pending_key_downs: GcRefCell::default(),
            pointer_interaction_value_map: RefCell::default(),
            pending_pointer_downs: GcRefCell::default(),
            is_contextmenu_triggered: Cell::new(false),
            event_counts: Cell::new(None),
            interaction_count: Cell::new(0),
        })
    }

    pub(crate) fn event_counts(&self, vm: &Vm) -> Ref<EventCounts> {
        if let Some(event_counts) = self.event_counts.get() {
            return event_counts;
        }
        let event_counts = EventCounts::create(vm);
        self.event_counts.set(Some(event_counts));
        event_counts
    }

    pub(crate) fn interaction_count(&self) -> u64 {
        self.interaction_count.get()
    }

    pub(crate) fn has_dispatched_input_event(&self) -> bool {
        self.has_dispatched_input_event.get()
    }

    fn take_pending_key_down(&self, vm: &Vm, code: u32) -> Option<Ref<PerformanceEventTiming>> {
        let mut pending_key_downs = self.pending_key_downs.borrow_mut(vm);
        let index = pending_key_downs.iter().position(|(key, _)| *key == code)?;
        Some(pending_key_downs.remove(index).1)
    }

    fn take_pending_pointer_down(&self, vm: &Vm, pointer_id: i32) -> Option<Ref<PerformanceEventTiming>> {
        let mut pending_pointer_downs = self.pending_pointer_downs.borrow_mut(vm);
        let index = pending_pointer_downs.iter().position(|(key, _)| *key == pointer_id)?;
        Some(pending_pointer_downs.remove(index).1)
    }

    fn pending_key_down(&self, code: u32) -> Option<Ref<PerformanceEventTiming>> {
        self.pending_key_downs
            .borrow()
            .iter()
            .find(|(key, _)| *key == code)
            .map(|(_, entry)| *entry)
    }

    fn set_pending_key_down(&self, vm: &Vm, code: u32, entry: Ref<PerformanceEventTiming>) {
        let mut pending_key_downs = self.pending_key_downs.borrow_mut(vm);
        match pending_key_downs.iter_mut().find(|(key, _)| *key == code) {
            Some(pending_key_down) => pending_key_down.1 = entry,
            None => pending_key_downs.push((code, entry)),
        }
    }

    fn pending_pointer_down(&self, pointer_id: i32) -> Option<Ref<PerformanceEventTiming>> {
        self.pending_pointer_downs
            .borrow()
            .iter()
            .find(|(key, _)| *key == pointer_id)
            .map(|(_, entry)| *entry)
    }

    fn set_pending_pointer_down(&self, vm: &Vm, pointer_id: i32, entry: Ref<PerformanceEventTiming>) {
        let mut pending_pointer_downs = self.pending_pointer_downs.borrow_mut(vm);
        match pending_pointer_downs.iter_mut().find(|(key, _)| *key == pointer_id) {
            Some(pending_pointer_down) => pending_pointer_down.1 = entry,
            None => pending_pointer_downs.push((pointer_id, entry)),
        }
    }

    fn pointer_interaction_value(&self, pointer_id: i32) -> Option<u64> {
        self.pointer_interaction_value_map
            .borrow()
            .iter()
            .find(|(key, _)| *key == pointer_id)
            .map(|(_, value)| *value)
    }

    fn set_pointer_interaction_value(&self, pointer_id: i32, value: u64) {
        let mut pointer_map = self.pointer_interaction_value_map.borrow_mut();
        match pointer_map.iter_mut().find(|(key, _)| *key == pointer_id) {
            Some(entry) => entry.1 = value,
            None => pointer_map.push((pointer_id, value)),
        }
    }

    fn remove_pointer_interaction_value(&self, pointer_id: i32) {
        self.pointer_interaction_value_map
            .borrow_mut()
            .retain(|(key, _)| *key != pointer_id);
    }
}

// https://w3c.github.io/event-timing/#increase-interaction-count
fn increase_interaction_count(state: &WindowEventTimingState) {
    // 1. Increase window's user interaction value value by a small number chosen by the user agent.
    state.user_interaction_value.set(state.user_interaction_value.get() + 7);

    // 2. Let interactionCount be window's interactionCount.
    // 3. Set interactionCount to interactionCount + 1.
    state.interaction_count.set(state.interaction_count.get() + 1);
}

// https://w3c.github.io/event-timing/#compute-interactionid
fn compute_interaction_id(vm: &Vm, event: &CppEvent, window: &CppWindow) -> u64 {
    // 1. If event's isTrusted attribute value is false, return 0.
    if !event.is_trusted() {
        return 0;
    }

    // 2. Let type be event's type attribute value.
    let r#type = event.r#type();
    // 3. If type is not one among keyup, compositionstart, input, pointercancel, pointerup, click, or contextmenu,
    //    return 0.
    if ![
        "keyup",
        "compositionstart",
        "input",
        "pointercancel",
        "pointerup",
        "click",
        "contextmenu",
    ]
    .iter()
    .any(|event_type| r#type == Utf16String::from_utf8(event_type))
    {
        return 0;
    }

    // 4. Let window be event's relevant global object.
    // NB: The window is passed in by the caller.
    let state = window.event_timing_state(vm);

    // 5. Let pendingKeyDowns be window's pending key downs.
    // 6. Let pointerMap be window's pointer interaction value map.
    // 7. Let pendingPointerDowns be window's pending pointer downs.

    // 8. If type is keyup:
    if r#type == Utf16String::from_utf8("keyup") {
        let Some(keyboard_event) = CppKeyboardEvent::from_event(event) else {
            return 0;
        };

        // 1. If event's isComposing attribute value is true, return 0.
        if keyboard_event.is_composing() {
            return 0;
        }

        // 2. Let code be event's keyCode attribute value.
        let code = keyboard_event.key_code();

        // 3. If pendingKeyDowns[code] does not exist, return 0.
        // 4. Let entry be pendingKeyDowns[code].
        // 9. Remove pendingKeyDowns[code].
        let Some(entry) = state.take_pending_key_down(vm, code) else {
            return 0;
        };

        // 5. Increase interaction count on window.
        increase_interaction_count(&state);

        // 6. Let interactionId be window's user interaction value value.
        let interaction_id = state.user_interaction_value.get();

        // 7. Set entry's interactionId to interactionId.
        entry.set_interaction_id(interaction_id);

        // 8. Add entry to window's entries to be queued.
        state.entries_to_be_queued.borrow_mut(vm).push(entry);

        // 10. Return interactionId.
        return interaction_id;
    }

    // 9. If type is compositionstart:
    if r#type == Utf16String::from_utf8("compositionstart") {
        // 1. For each entry in the values of pendingKeyDowns:
        // 2. Clear pendingKeyDowns.
        let mut pending_key_downs = state.pending_key_downs.borrow_mut(vm);
        for (_, entry) in pending_key_downs.drain(..) {
            // 1. Append entry to window's entries to be queued.
            state.entries_to_be_queued.borrow_mut(vm).push(entry);
        }

        // 3. Return 0.
        return 0;
    }

    // 10. If type is input:
    if r#type == Utf16String::from_utf8("input") {
        // 1. If event is not an instance of InputEvent, return 0.
        // NOTE: This check is done to exclude Events for which the type is input but that are not about modified text
        //       content.
        let Some(input_event) = CppInputEvent::from_event(event) else {
            return 0;
        };

        // 2. If event's isComposing attribute value is false, return 0.
        if !input_event.is_composing() {
            return 0;
        }

        // 3. Increase interaction count on window.
        increase_interaction_count(&state);

        // 4. Return window's user interaction value.
        return state.user_interaction_value.get();
    }

    // 11. Otherwise (type is pointercancel, pointerup, click, or contextmenu):
    // 1. Let pointerId be event's pointerId attribute value.
    // AD-HOC: We dispatch click and contextmenu events as MouseEvents rather than PointerEvents, so they have no
    //         pointerId. They come from the mouse, so they use the pointerId of the mouse instead.
    let pointer_id = if let Some(pointer_event) = CppPointerEvent::from_event(event) {
        pointer_event.pointer_id()
    } else if is_mouse_event(event) {
        MOUSE_POINTER_ID
    } else {
        return 0;
    };

    // 2. If type is click:
    if r#type == Utf16String::from_utf8("click") {
        // 1. If pointerMap[pointerId] does not exist, return 0.
        // 2. Let value be pointerMap[pointerId].
        let Some(value) = state.pointer_interaction_value(pointer_id) else {
            return 0;
        };

        // 3. Remove pointerMap[pointerId].
        state.remove_pointer_interaction_value(pointer_id);

        // 4. Return value.
        return value;
    }

    // 3. Assert that type is pointerup, pointercancel, or contextmenu.
    assert!(
        r#type == Utf16String::from_utf8("pointerup")
            || r#type == Utf16String::from_utf8("pointercancel")
            || r#type == Utf16String::from_utf8("contextmenu")
    );

    // 4. If pendingPointerDowns[pointerId] does not exist:
    // 5. Let pointerDownEntry be pendingPointerDowns[pointerId].
    // 6. Assert that pointerDownEntry is a PerformanceEventTiming entry.
    // 9. Remove pendingPointerDowns[pointerId].
    let Some(pointer_down_entry) = state.take_pending_pointer_down(vm, pointer_id) else {
        // 1. If type is contextmenu, return window's user interaction value.
        if r#type == Utf16String::from_utf8("contextmenu") {
            return state.user_interaction_value.get();
        }

        // 2. If type is pointerup and window's is contextmenu triggered flag is true:
        if r#type == Utf16String::from_utf8("pointerup") && state.is_contextmenu_triggered.get() {
            // 1. Set window's is contextmenu triggered flag to false.
            state.is_contextmenu_triggered.set(false);

            // 2. Return window's user interaction value.
            return state.user_interaction_value.get();
        }

        // 3. Otherwise, return 0.
        return 0;
    };

    // 7. If type is pointerup or contextmenu:
    if r#type == Utf16String::from_utf8("pointerup") || r#type == Utf16String::from_utf8("contextmenu") {
        // 1. Increase interaction count on window.
        increase_interaction_count(&state);

        // 2. Set pointerMap[pointerId] to window's user interaction value.
        state.set_pointer_interaction_value(pointer_id, state.user_interaction_value.get());

        // 3. Set pointerDownEntry's interactionId to pointerMap[pointerId].
        pointer_down_entry.set_interaction_id(state.user_interaction_value.get());
    }

    // 8. Append pointerDownEntry to window’s entries to be queued.
    state.entries_to_be_queued.borrow_mut(vm).push(pointer_down_entry);

    // 10. If type is contextmenu, set window's is contextmenu triggered to true.
    if r#type == Utf16String::from_utf8("contextmenu") {
        state.is_contextmenu_triggered.set(true);
    }

    // 11. If type is pointercancel, return 0.
    if r#type == Utf16String::from_utf8("pointercancel") {
        return 0;
    }

    // 12. Return pointerMap[pointerId].
    state.pointer_interaction_value(pointer_id).unwrap_or(0)
}

// https://w3c.github.io/event-timing/#initialize-event-timing
fn initialize_event_timing(
    vm: &Vm,
    event: &CppEvent,
    processing_start: f64,
    interaction_id: u64,
) -> Ref<PerformanceEventTiming> {
    // 1. If the algorithm to determine if event should be considered for Event Timing returns false, then return null.
    // NB: The caller only initializes event timing for an event that is considered for Event Timing.

    // 2. Let timingEntry be a new PerformanceEventTiming object with event's relevant realm.
    // 3. Set timingEntry's name to event's type attribute value.
    // 4. Set timingEntry's entryType to "event".
    // 5. Set timingEntry's startTime to event's timeStamp attribute value.
    // 6. Set timingEntry's processingStart to processingStart.
    // 7. Set timingEntry's cancelable to event's cancelable attribute value.
    // 8. Set timingEntry's interactionId to interactionId.
    // 9. Return timingEntry.
    PerformanceEventTiming::create(
        vm,
        event.r#type(),
        event.time_stamp(),
        processing_start,
        event.cancelable(),
        interaction_id,
    )
}

// https://w3c.github.io/event-timing/#finalize-event-timing
fn finalize_event_timing(
    vm: &Vm,
    timing_entry: Ref<PerformanceEventTiming>,
    event: &CppEvent,
    relevant_global: &CppWindow,
    target_node: Option<CppCell>,
    target_selector: Utf16String,
    processing_end: f64,
) {
    // 1. If timingEntry is null, then return.
    // 2. Let relevantGlobal be target's relevant global object.
    // 3. If relevantGlobal does not implement Window, return.
    // NB: The caller has done these steps.
    let state = relevant_global.event_timing_state(vm);

    // 4. Set timingEntry's processingEnd to processingEnd.
    timing_entry.set_processing_end(processing_end);

    // 5. Assert that target implements Node.
    // NOTE: This assertion holds due to the types of events supported by the Event Timing API.
    // NB: Events of these types can also be dispatched at the Window or the Document, which have no element to time.

    // 6. Set timingEntry's eventTarget to target.
    // NOTE: This will set eventTarget to the last event target. So if retargeting occurs, the last target, closest to
    //       the root, will be used.
    timing_entry.set_event_target(vm, target_node);

    // 7. Set timingEntry's targetSelector to the result of running the algorithm to generate a CSS selector with
    //    target as input.
    timing_entry.set_target_selector(target_selector);

    // 8. If event's type attribute value is pointerdown:
    let r#type = event.r#type();
    if r#type == Utf16String::from_utf8("pointerdown") {
        // 1. Let pendingPointerDowns be relevantGlobal's pending pointer downs.
        // 2. Let pointerId be event's pointerId.
        let pointer_id = CppPointerEvent::from_event(event).map_or(0, |pointer_event| pointer_event.pointer_id());

        // 3. If pendingPointerDowns[pointerId] exists:
        if let Some(previous_pointer_down_entry) = state.pending_pointer_down(pointer_id) {
            // 1. Let previousPointerDownEntry be pendingPointerDowns[pointerId].
            // 2. Add previousPointerDownEntry to relevantGlobal's entries to be queued.
            state
                .entries_to_be_queued
                .borrow_mut(vm)
                .push(previous_pointer_down_entry);
        }

        // 4. Set pendingPointerDowns[pointerId] to timingEntry.
        state.set_pending_pointer_down(vm, pointer_id, timing_entry);

        // 5. Set relevantGlobal's is contextmenu triggered to false.
        state.is_contextmenu_triggered.set(false);
    }
    // 9. Otherwise, if event's type attribute value is keydown:
    else if let Some(keyboard_event) = CppKeyboardEvent::from_event(event)
        && r#type == Utf16String::from_utf8("keydown")
    {
        // 1. If event's isComposing attribute value is true:
        if keyboard_event.is_composing() {
            // 1. Append timingEntry to relevantGlobal's entries to be queued.
            state.entries_to_be_queued.borrow_mut(vm).push(timing_entry);

            // 2. Return.
            return;
        }

        // 2. Let pendingKeyDowns be relevantGlobal's pending key downs.
        // 3. Let code be event's keyCode attribute value.
        let code = keyboard_event.key_code();

        // 4. If pendingKeyDowns[code] exists:
        if let Some(previous_key_down_entry) = state.pending_key_down(code) {
            // 1. Let previousKeyDownEntry be pendingKeyDowns[code].
            // 2. If code is not 229:
            if code != 229 {
                // 1. Increase relevantGlobal's user interaction value value by a small number chosen by the user
                //    agent.
                state.user_interaction_value.set(state.user_interaction_value.get() + 7);

                // 2. Set previousKeyDownEntry's interactionId to relevantGlobal's user interaction value.
                previous_key_down_entry.set_interaction_id(state.user_interaction_value.get());
            }
            // NOTE: 229 is a special case since it corresponds to IME keyboard events. Sometimes multiple of these
            //       are sent by the user agent, and they do not correspond to holding a key down repeatedly.

            // 3. Add previousKeyDownEntry to relevantGlobal's entries to be queued.
            state.entries_to_be_queued.borrow_mut(vm).push(previous_key_down_entry);
        }

        // 5. Set pendingKeyDowns[code] to timingEntry.
        state.set_pending_key_down(vm, code, timing_entry);
    }
    // 10. Otherwise:
    else {
        // 1. Append timingEntry to relevantGlobal's entries to be queued.
        state.entries_to_be_queued.borrow_mut(vm).push(timing_entry);
    }

    // AD-HOC: The duration of the entry is set by the next rendering update, which is asked for here in case nothing
    //         else asks for one.
    // SAFETY: The Window is live.
    unsafe { web_event_timing_request_frame(relevant_global.as_ptr()) };
}

// https://w3c.github.io/event-timing/#set-event-timing-entry-duration
fn set_event_timing_entry_duration(
    vm: &Vm,
    timing_entry: Ref<PerformanceEventTiming>,
    window: &CppWindow,
    rendering_timestamp: f64,
) {
    let state = window.event_timing_state(vm);

    // 1. If timingEntry's duration attribute value is nonzero, return.
    // AD-HOC: A duration that rounds to zero is a duration that has been set all the same. Otherwise, a pending
    //         pointerdown or keydown entry would be given a duration again by every later rendering update until its
    //         duration rounds to a nonzero value, and its event would be counted in eventCounts each time.
    if timing_entry.has_duration() {
        return;
    }

    let entry = AnyPerformanceEntry::PerformanceEventTiming(timing_entry);

    // 2. Let start be timingEntry's startTime attribute value.
    let start = entry.start_time();

    // 3. Set timingEntry's duration to a DOMHighResTimeStamp resulting from renderingTimestamp - start, with
    //    granularity of 8ms or less.
    timing_entry.set_duration(((rendering_timestamp - start) / 8.0).round() * 8.0);

    // 4. Let name be timingEntry's name attribute value.
    let name = entry.name();

    // 5. Perform the following steps to update the event counts:
    // 1. Let eventCounts be window's eventCounts.
    // 2. Assert that eventCounts contains name.
    // 3. Set eventCounts[name] to eventCounts[name] + 1.
    state.event_counts(vm).increment(&name);

    // 6. If window's has dispatched input event is false, and timingEntry's interactionId is not 0, run the following
    //    steps:
    if !state.has_dispatched_input_event.get() && timing_entry.interaction_id.get() != 0 {
        // 1. Let firstInputEntry be a copy of timingEntry.
        // 2. Set firstInputEntry's entryType to "first-input".
        let first_input_entry = AnyPerformanceEntry::PerformanceEventTiming(
            timing_entry.copy_with_entry_type(vm, Utf16String::from_utf8("first-input")),
        );

        // 3. Queue firstInputEntry.
        let timeline = PerformanceTimeline::of(vm, window.realm());
        timeline.queue_performance_entry(vm, &first_input_entry);
        timeline.add_performance_entry(vm, &first_input_entry, true);

        // 4. Set window's has dispatched input event to true.
        state.has_dispatched_input_event.set(true);
    }
}

fn has_pending_event_timing_entries(vm: &Vm, window: &CppWindow) -> bool {
    let state = window.event_timing_state(vm);
    if !state.entries_to_be_queued.borrow().is_empty() {
        return true;
    }
    state
        .pending_pointer_downs
        .borrow()
        .iter()
        .any(|(_, entry)| !entry.has_duration())
        || state
            .pending_key_downs
            .borrow()
            .iter()
            .any(|(_, entry)| !entry.has_duration())
}

// https://w3c.github.io/event-timing/#dispatch-pending-event-timing-entries
fn dispatch_pending_event_timing_entries(vm: &Vm, window: &CppWindow) {
    // 1. Let window be doc's relevant global object.
    let state = window.event_timing_state(vm);

    // OPTIMIZATION: There is nothing to do while no event has been timed.
    if state.entries_to_be_queued.borrow().is_empty()
        && state.pending_pointer_downs.borrow().is_empty()
        && state.pending_key_downs.borrow().is_empty()
    {
        return;
    }

    // 2. Let renderingTimestamp be the current high resolution time.
    // SAFETY: The Window's global object is live.
    let rendering_timestamp = unsafe {
        web_high_resolution_time_current_high_resolution_time(window.realm().global_object().as_ptr().cast())
    };

    // 3. For each timingEntry in window's entries to be queued:
    // 4. Set window's entries to be queued to an empty list.
    let entries_to_be_queued = MarkedVec::new(vm);
    entries_to_be_queued.extend_from_slice(&state.entries_to_be_queued.borrow());
    state.entries_to_be_queued.borrow_mut(vm).clear();
    let timeline = PerformanceTimeline::of(vm, window.realm());
    for timing_entry in entries_to_be_queued.values().iter().copied() {
        // 1. Set event timing entry duration passing timingEntry, window, and renderingTimestamp.
        set_event_timing_entry_duration(vm, timing_entry, window, rendering_timestamp);

        // 2. If timingEntry's duration attribute value is greater than or equal to 16, then queue timingEntry.
        let entry = AnyPerformanceEntry::PerformanceEventTiming(timing_entry);
        if entry.duration() >= 16.0 {
            timeline.queue_performance_entry(vm, &entry);
            timeline.add_performance_entry(vm, &entry, true);
        }
    }

    // 5. For each pendingPointerDownEntry in the values from window's pending pointer downs:
    let pending_pointer_downs = MarkedVec::new(vm);
    for (_, entry) in state.pending_pointer_downs.borrow().iter() {
        pending_pointer_downs.push(*entry);
    }
    for pending_pointer_down_entry in pending_pointer_downs.values().iter().copied() {
        // 1. Set event timing entry duration passing pendingPointerDownEntry, window, and renderingTimestamp.
        set_event_timing_entry_duration(vm, pending_pointer_down_entry, window, rendering_timestamp);
    }

    // 6. For each pendingKeyDownEntry in the values from window's pending key downs:
    let pending_key_downs = MarkedVec::new(vm);
    for (_, entry) in state.pending_key_downs.borrow().iter() {
        pending_key_downs.push(*entry);
    }
    for pending_key_down_entry in pending_key_downs.values().iter().copied() {
        // 1. Set event timing entry duration passing pendingKeyDownEntry, window, and renderingTimestamp.
        set_event_timing_entry_duration(vm, pending_key_down_entry, window, rendering_timestamp);
    }
}

/// https://w3c.github.io/event-timing/#compute-interactionid
///
/// # Safety
///
/// `vm` must be the live VM, `window` a live Window, and `event` a live Event.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_compute_interaction_id(
    vm: *mut c_void,
    window: *mut c_void,
    event: *mut c_void,
) -> u64 {
    // SAFETY: The caller passes the live VM, a live Window and a live Event.
    let (vm, window, event) = unsafe {
        (
            &*vm.cast::<Vm>(),
            CppWindow::from_raw(window),
            CppEvent::from_raw(event),
        )
    };
    compute_interaction_id(vm, &event, &window)
}

/// https://w3c.github.io/event-timing/#initialize-event-timing
///
/// # Safety
///
/// `vm` must be the live VM, and `event` a live Event.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_initialize_event_timing(
    vm: *mut c_void,
    event: *mut c_void,
    processing_start: f64,
    interaction_id: u64,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live Event.
    let (vm, event) = unsafe { (&*vm.cast::<Vm>(), CppEvent::from_raw(event)) };
    cell_into_abi(initialize_event_timing(vm, &event, processing_start, interaction_id))
}

/// https://w3c.github.io/event-timing/#finalize-event-timing
///
/// # Safety
///
/// `vm` must be the live VM, `timing_entry` the live cell of a PerformanceEventTiming, `relevant_global` a live Window,
/// `event` a live Event, `target_node` null or a live Node, and `target_selector` a raw
/// AK::Utf16String that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_finalize_event_timing(
    vm: *mut c_void,
    timing_entry: *mut c_void,
    event: *mut c_void,
    relevant_global: *mut c_void,
    target_node: *mut c_void,
    target_selector: usize,
    processing_end: f64,
) {
    // SAFETY: The caller passes the live VM, a live entry, a live Event, a live Window and null or a live Node, and hands
    //         over the selector.
    let (vm, timing_entry, event, relevant_global, target_node, target_selector) = unsafe {
        (
            &*vm.cast::<Vm>(),
            cell_from_abi::<PerformanceEventTiming>(timing_entry),
            CppEvent::from_raw(event),
            CppWindow::from_raw(relevant_global),
            (!target_node.is_null()).then(|| CppCell::from_raw(target_node)),
            Utf16String::from_raw_owned(target_selector),
        )
    };
    finalize_event_timing(
        vm,
        timing_entry,
        &event,
        &relevant_global,
        target_node,
        target_selector,
        processing_end,
    );
}

/// # Safety
///
/// `vm` must be the live VM, and `window` a live Window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_has_pending_event_timing_entries(
    vm: *mut c_void,
    window: *mut c_void,
) -> bool {
    // SAFETY: The caller passes the live VM and a live Window.
    let (vm, window) = unsafe { (&*vm.cast::<Vm>(), CppWindow::from_raw(window)) };
    has_pending_event_timing_entries(vm, &window)
}

/// https://w3c.github.io/event-timing/#dispatch-pending-event-timing-entries
///
/// # Safety
///
/// `vm` must be the live VM, and `window` a live Window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_dispatch_pending_event_timing_entries(
    vm: *mut c_void,
    window: *mut c_void,
) {
    // SAFETY: The caller passes the live VM and a live Window.
    let (vm, window) = unsafe { (&*vm.cast::<Vm>(), CppWindow::from_raw(window)) };
    dispatch_pending_event_timing_entries(vm, &window);
}

/// https://w3c.github.io/event-timing/#sec-modifications-HTML
///
/// # Safety
///
/// `vm` must be the live VM, and `window` a live Window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_has_dispatched_input_event(
    vm: *mut c_void,
    window: *mut c_void,
) -> bool {
    // SAFETY: The caller passes the live VM and a live Window.
    let (vm, window) = unsafe { (&*vm.cast::<Vm>(), CppWindow::from_raw(window)) };
    window.event_timing_state(vm).has_dispatched_input_event()
}
