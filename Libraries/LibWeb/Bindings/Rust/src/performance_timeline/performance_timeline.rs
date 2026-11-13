/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The performance timeline, https://w3c.github.io/performance-timeline/#performance-timeline

use core::cell::Cell;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, Realm, Trace, Vm};

use crate::bindings::CallContext;
use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{
    Ref, realm_is_window, report_an_exception, with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{TaskSource, queue_a_global_task, queue_a_task};
use crate::bindings::realm_data::realm_data;
use crate::high_resolution_time::performance::Performance;
use crate::performance_timeline::entry_types::supported_entry_types;
use crate::performance_timeline::performance_entry::{
    AnyPerformanceEntry, filter_buffer_by_name_and_type, sort_in_chronological_order,
};
use crate::performance_timeline::performance_entry_tuple::PerformanceEntryTuple;
use crate::performance_timeline::performance_observer::PerformanceObserver;
use crate::performance_timeline::performance_observer_entry_list::PerformanceObserverEntryList;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceTimeline {
    realm: Gc<Realm>,

    // https://w3c.github.io/hr-time/#dom-windoworworkerglobalscope-performance
    performance: Cell<Option<Ref<Performance>>>,

    // https://w3c.github.io/performance-timeline/#performance-timeline
    // Each global object has:
    // - a performance observer task queued flag
    performance_observer_task_queued: Cell<bool>,

    // - a list of registered performance observer objects that is initially empty
    registered_performance_observer_objects: GcRefCell<Vec<Ref<PerformanceObserver>>>,

    // https://w3c.github.io/performance-timeline/#dfn-performance-entry-buffer-map
    // a performance entry buffer map map, keyed on a DOMString, representing the entry type to which the buffer belongs. The map's value is the following tuple:
    performance_entry_buffer_map: GcRefCell<Vec<PerformanceEntryTuple>>,

    // https://w3c.github.io/resource-timing/#sec-extensions-performance-interface
    // Each ECMAScript global environment has:
    // https://w3c.github.io/resource-timing/#dfn-resource-timing-buffer-size-limit
    // A resource timing buffer size limit which should initially be 250 or greater.
    // The recommended minimum number of PerformanceResourceTiming objects is 250, though this may be changed by the
    // user agent. setResourceTimingBufferSize can be called to request a change to this limit.
    resource_timing_buffer_size_limit: Cell<u32>,

    // https://w3c.github.io/resource-timing/#dfn-resource-timing-buffer-full-event-pending-flag
    // A resource timing buffer full event pending flag which is initially false.
    resource_timing_buffer_full_event_pending: Cell<bool>,

    // https://w3c.github.io/resource-timing/#dfn-resource-timing-secondary-buffer-current-size
    // A resource timing secondary buffer current size which is initially 0.
    // https://w3c.github.io/resource-timing/#dfn-resource-timing-secondary-buffer
    // A resource timing secondary buffer to store PerformanceResourceTiming objects that is initially empty.
    resource_timing_secondary_buffer: GcRefCell<Vec<AnyPerformanceEntry>>,
}

static PERFORMANCE_TIMELINE_CLASS: EmbedderClass<PerformanceTimeline> = EmbedderClass::new("PerformanceTimeline");

impl EmbedderValue for PerformanceTimeline {
    const CLASS: &'static EmbedderClass<Self> = &PERFORMANCE_TIMELINE_CLASS;
}

pub(crate) type PerformanceTimelineRef = Gc<EmbedderCell<PerformanceTimeline>>;

impl PerformanceTimeline {
    /// Every world of the global object shares its timeline.
    pub(crate) fn of(vm: &Vm, realm: Gc<Realm>) -> PerformanceTimelineRef {
        let realm = relevant_settings_object(realm.global_object()).realm();
        let data = realm_data(vm, realm);
        if let Some(timeline) = data.performance_timeline.get() {
            return timeline;
        }
        let timeline = PERFORMANCE_TIMELINE_CLASS.allocate(vm, |defer_gc| Self {
            realm,
            performance: Cell::new(None),
            performance_observer_task_queued: Cell::new(false),
            registered_performance_observer_objects: GcRefCell::default(),
            performance_entry_buffer_map: GcRefCell::new(
                supported_entry_types(realm_is_window(realm))
                    .map(|supported_entry_type| PerformanceEntryTuple::new(defer_gc, supported_entry_type))
                    .collect(),
            ),
            resource_timing_buffer_size_limit: Cell::new(250),
            resource_timing_buffer_full_event_pending: Cell::new(false),
            resource_timing_secondary_buffer: GcRefCell::default(),
        });
        data.performance_timeline.set(Some(timeline));
        timeline
    }

    // https://w3c.github.io/hr-time/#dom-windoworworkerglobalscope-performance
    pub(crate) fn performance(&self, vm: &Vm) -> Ref<Performance> {
        if let Some(performance) = self.performance.get() {
            return performance;
        }
        let performance = Performance::create(vm, self.realm);
        self.performance.set(Some(performance));
        performance
    }

    /// https://w3c.github.io/performance-timeline/#dfn-relevant-performance-entry-tuple
    pub(crate) fn with_relevant_performance_entry_tuple<R>(
        &self,
        entry_type: &Utf16String,
        steps: impl FnOnce(&PerformanceEntryTuple) -> R,
    ) -> R {
        // 1. Let map be the performance entry buffer map associated with globalObject.
        // 2. Return the result of getting the value of an entry from map, given entryType as the key.
        let map = self.performance_entry_buffer_map.borrow();
        steps(Self::tuple_of(&map, entry_type))
    }

    fn with_relevant_performance_entry_tuple_mut<R>(
        &self,
        vm: &Vm,
        entry_type: &Utf16String,
        steps: impl FnOnce(&mut PerformanceEntryTuple) -> R,
    ) -> R {
        let mut map = self.performance_entry_buffer_map.borrow_mut(vm);
        let tuple = map
            .iter_mut()
            .find(|tuple| tuple.entry_type == *entry_type)
            .expect("the entry type is a supported entry type");
        steps(tuple)
    }

    fn tuple_of<'a>(map: &'a [PerformanceEntryTuple], entry_type: &Utf16String) -> &'a PerformanceEntryTuple {
        map.iter()
            .find(|tuple| tuple.entry_type == *entry_type)
            .expect("the entry type is a supported entry type")
    }

    // https://w3c.github.io/performance-timeline/#dfn-queue-a-performanceentry
    pub(crate) fn queue_performance_entry(&self, vm: &Vm, new_entry: &AnyPerformanceEntry) {
        // 1. Let interested observers be an initially empty set of PerformanceObserver objects.
        let interested_observers = MarkedVec::new(vm);

        // 2. Let entryType be newEntry’s entryType value.
        let entry_type = new_entry.entry_type();

        // 3. Let relevantGlobal be newEntry's relevant global object.
        // NB: Already is `self`.

        // 4. For each registered performance observer regObs in relevantGlobal's list of registered performance observer
        //    objects:
        for registered_observer in self.registered_performance_observer_objects.borrow().iter() {
            // 1. If regObs's options list contains a PerformanceObserverInit options whose entryTypes member includes entryType
            //    or whose type member equals to entryType:
            let options_list = registered_observer.options_list();
            let options = options_list.iter().find(|options| match &options.entry_types {
                Some(entry_types) => entry_types.contains(&entry_type),
                None => options.r#type.as_ref() == Some(&entry_type),
            });

            if let Some(options) = options {
                // 1. If should add entry with newEntry and options returns true, append regObs's observer to interested observers.
                if new_entry.should_add_entry(Some(options)) {
                    interested_observers.push(*registered_observer);
                }
            }
        }

        // 5. For each observer in interested observers:
        for observer in interested_observers.values().iter() {
            // 1. Append newEntry to observer's observer buffer.
            observer.append_to_observer_buffer(vm, *new_entry);
        }

        // AD-HOC: Steps 6-9 are not here because other engines do not add to the performance entry buffer when queuing
        //         the performance observer task. The users of the Performance Timeline specification also do not expect
        //         this function to add to the entry buffer, instead queuing the observer task, then adding to the entry
        //         buffer separately.

        // OPTIMIZATION: The PerformanceObserver task delivers only what observers have buffered. An entry that no observer
        //               is interested in buffers nothing, so it doesn't queue the task. A document's initial visibility
        //               state entry, e.g., then doesn't queue a task for every new document.
        if interested_observers.is_empty() {
            return;
        }

        // 10. Queue the PerformanceObserver task with relevantGlobal as input.
        self.queue_the_performance_observer_task(vm);
    }

    // https://w3c.github.io/performance-timeline/#dfn-queue-a-performanceentry
    // AD-HOC: This is a separate function because the users of this specification queues PerformanceObserver tasks and add
    //         to the entry buffer separately.
    pub(crate) fn add_performance_entry(
        &self,
        vm: &Vm,
        new_entry: &AnyPerformanceEntry,
        check_if_performance_buffer_is_full: bool,
    ) {
        // 6. Let tuple be the relevant performance entry tuple of entryType and relevantGlobal.
        self.with_relevant_performance_entry_tuple_mut(vm, &new_entry.entry_type(), |tuple| {
            // AD-HOC: We have a custom flag to always append to the buffer by default, as other performance specs do this by default
            //         (either they don't have a limit, or they check the limit themselves). This flag allows compatibility for specs
            //         that rely do and don't rely on this.
            let mut is_buffer_full = false;
            let mut should_add = true;

            if check_if_performance_buffer_is_full {
                // 7. Let isBufferFull be the return value of the determine if a performance entry buffer is full algorithm with tuple
                //    as input.
                is_buffer_full = tuple.is_full();

                // 8. Let shouldAdd be the result of should add entry with newEntry as input.
                should_add = new_entry.should_add_entry(None);
            }

            // 9. If isBufferFull is false and shouldAdd is true, append newEntry to tuple's performance entry buffer.
            if !is_buffer_full && should_add {
                tuple.performance_entry_buffer.push(*new_entry);
            }
        });
    }

    pub(crate) fn clear_performance_entry_buffer(&self, vm: &Vm, entry_type: &Utf16String) {
        self.with_relevant_performance_entry_tuple_mut(vm, entry_type, |tuple| tuple.performance_entry_buffer.clear());
    }

    pub(crate) fn remove_entries_from_performance_entry_buffer(
        &self,
        vm: &Vm,
        entry_type: &Utf16String,
        entry_name: &Utf16String,
    ) {
        self.with_relevant_performance_entry_tuple_mut(vm, entry_type, |tuple| {
            tuple
                .performance_entry_buffer
                .retain(|entry| entry.name() != *entry_name);
        });
    }

    // https://w3c.github.io/performance-timeline/#dfn-filter-buffer-map-by-name-and-type
    pub(crate) fn filter_buffer_map_by_name_and_type<'vm>(
        &self,
        cx: &CallContext<'vm>,
        name: Option<&Utf16String>,
        r#type: Option<&Utf16String>,
    ) -> MarkedVec<'vm, AnyPerformanceEntry> {
        // 1. Let result be an initially empty list.
        let result = MarkedVec::new(cx.vm());

        // 2. Let map be the performance entry buffer map associated with the relevant global object of this.
        let map = self.performance_entry_buffer_map.borrow();

        // 3. Let tuple list be an empty list.
        // 4. If type is not null, append the result of getting the value of entry on map given type as key to tuple list.
        //    Otherwise, assign the result of get the values on map to tuple list.
        let tuple_list = map
            .iter()
            .filter(|tuple| r#type.is_none_or(|r#type| tuple.entry_type == *r#type));

        // 5. For each tuple in tuple list, run the following steps:
        for tuple in tuple_list {
            // 1. Let buffer be tuple's performance entry buffer.
            let buffer = &tuple.performance_entry_buffer;

            // 2. If tuple's availableFromTimeline is false, continue to the next tuple.
            if !tuple.available_from_timeline {
                continue;
            }

            // 3. Let entries be the result of running filter buffer by name and type with buffer, name and type as inputs.
            let entries = filter_buffer_by_name_and_type(cx, buffer, name, r#type);

            // 4. For each entry in entries, append entry to result.
            result.extend_from_slice(&entries.values());
        }

        // 6. Sort results's entries in chronological order with respect to startTime
        sort_in_chronological_order(&result);

        // 7. Return result.
        result
    }

    pub(crate) fn register_performance_observer(&self, vm: &Vm, observer: Ref<PerformanceObserver>) {
        if !self.has_registered_performance_observer(observer) {
            self.registered_performance_observer_objects
                .borrow_mut(vm)
                .push(observer);
        }
    }

    pub(crate) fn unregister_performance_observer(&self, vm: &Vm, observer: Ref<PerformanceObserver>) {
        self.registered_performance_observer_objects
            .borrow_mut(vm)
            .retain(|registered_observer| *registered_observer != observer);
    }

    pub(crate) fn has_registered_performance_observer(&self, observer: Ref<PerformanceObserver>) -> bool {
        self.registered_performance_observer_objects
            .borrow()
            .contains(&observer)
    }

    // https://w3c.github.io/performance-timeline/#dfn-queue-the-performanceobserver-task
    pub(crate) fn queue_the_performance_observer_task(&self, vm: &Vm) {
        // 1. If relevantGlobal's performance observer task queued flag is set, terminate these steps.
        if self.performance_observer_task_queued.get() {
            return;
        }

        // 2. Set relevantGlobal's performance observer task queued flag.
        self.performance_observer_task_queued.set(true);

        // 3. Queue a task that consists of running the following substeps. The task source for the queued task is the performance
        //    timeline task source.
        queue_a_global_task(
            vm,
            self.realm.global_object(),
            TaskSource::PerformanceTimeline,
            self.realm,
            |vm, realm| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    Self::of(vm, realm).run_performance_observer_task(vm);
                });
            },
        );
    }

    fn run_performance_observer_task(&self, vm: &Vm) {
        // 1. Unset performance observer task queued flag of relevantGlobal.
        self.performance_observer_task_queued.set(false);

        // 2. Let notifyList be a copy of relevantGlobal's list of registered performance observer objects.
        let notify_list = MarkedVec::new(vm);
        notify_list.extend_from_slice(&self.registered_performance_observer_objects.borrow());

        // 3. For each registered performance observer object registeredObserver in notifyList, run these steps:
        for registered_observer in notify_list.values().iter() {
            // 1. Let po be registeredObserver's observer.
            // 2. Let entries be a copy of po’s observer buffer.
            // 4. Empty po’s observer buffer.
            let entries = registered_observer.take_observer_buffer(vm);

            // 3. If entries is empty, return.
            // FIXME: Do they mean `continue`?
            if entries.is_empty() {
                continue;
            }

            // 5. Let observerEntryList be a new PerformanceObserverEntryList, with its entry list set to entries.
            let observer_entry_list = PerformanceObserverEntryList::create(vm, registered_observer.realm(), &entries);

            // 6. Let droppedEntriesCount be null.
            let mut dropped_entries_count = None;

            // 7. If po's requires dropped entries is set, perform the following steps:
            if registered_observer.requires_dropped_entries() {
                // 1. Set droppedEntriesCount to 0.
                let mut count = 0;

                // 2. For each PerformanceObserverInit item in registeredObserver's options list:
                let map = self.performance_entry_buffer_map.borrow();
                for item in registered_observer.options_list().iter() {
                    // 1. For each DOMString entryType that appears either as item's type or in item's entryTypes:
                    for r#type in item.r#type.iter().chain(item.entry_types.iter().flatten()) {
                        // 1. Let map be relevantGlobal's performance entry buffer map.
                        // 2. Let tuple be the result of getting the value of entry on map given entryType as key.
                        let tuple = Self::tuple_of(&map, r#type);

                        // 3. Increase droppedEntriesCount by tuple's dropped entries count.
                        count += tuple.dropped_entries_count;
                    }
                }
                dropped_entries_count = Some(count);

                // 3. Set po's requires dropped entries to false.
                registered_observer.unset_requires_dropped_entries();
            }

            // 8. Let callbackOptions be a PerformanceObserverCallbackOptions with its droppedEntriesCount set to
            //    droppedEntriesCount if droppedEntriesCount is not null, otherwise unset.
            // 9. Call po’s observer callback with observerEntryList as the first argument, with po as the second
            //    argument and as callback this value, and with callbackOptions as the third argument.
            //    If this throws an exception, report the exception.
            if let Err(throw) = PerformanceObserver::invoke_callback(
                *registered_observer,
                vm,
                observer_entry_list,
                dropped_entries_count,
            ) {
                report_an_exception(self.realm, throw.value());
            }
        }
    }

    pub(crate) fn set_resource_timing_buffer_size_limit(&self, value: u32) {
        self.resource_timing_buffer_size_limit.set(value);
    }

    // https://w3c.github.io/resource-timing/#dfn-add-a-performanceresourcetiming-entry
    pub(crate) fn add_resource_timing_entry(&self, vm: &Vm, new_entry: &AnyPerformanceEntry) {
        // 1. If can add resource timing entry returns true and resource timing buffer full event pending flag is false,
        //    run the following substeps:
        if self.can_add_resource_timing_entry() && !self.resource_timing_buffer_full_event_pending.get() {
            // a. Add new entry to the performance entry buffer.
            // b. Increase resource timing buffer current size by 1.
            self.add_performance_entry(vm, new_entry, false);

            // c. Return.
            return;
        }

        // 2. If resource timing buffer full event pending flag is false, run the following substeps:
        if !self.resource_timing_buffer_full_event_pending.get() {
            // a. Set resource timing buffer full event pending flag to true.
            self.resource_timing_buffer_full_event_pending.set(true);

            // b. Queue a task on the performance timeline task source to run fire a buffer full event.
            queue_a_task(
                vm,
                self.realm,
                TaskSource::PerformanceTimeline,
                self.realm,
                |vm, realm| {
                    Self::of(vm, realm).fire_resource_timing_buffer_full_event(vm);
                },
            );
        }

        // 3. Add new entry to the resource timing secondary buffer.
        // 4. Increase resource timing secondary buffer current size by 1.
        self.resource_timing_secondary_buffer.borrow_mut(vm).push(*new_entry);
    }

    // https://w3c.github.io/resource-timing/#dfn-can-add-resource-timing-entry
    fn can_add_resource_timing_entry(&self) -> bool {
        // 1. If resource timing buffer current size is smaller than resource timing buffer size limit, return true.
        // 2. Return false.
        self.resource_timing_buffer_current_size() < self.resource_timing_buffer_size_limit.get() as usize
    }

    // https://w3c.github.io/resource-timing/#dfn-resource-timing-buffer-current-size
    fn resource_timing_buffer_current_size(&self) -> usize {
        // A resource timing buffer current size which is initially 0.
        self.with_relevant_performance_entry_tuple(&Utf16String::from_utf8("resource"), |tuple| {
            tuple.performance_entry_buffer.len()
        })
    }

    // https://w3c.github.io/resource-timing/#dfn-fire-a-buffer-full-event
    fn fire_resource_timing_buffer_full_event(&self, vm: &Vm) {
        // 1. While resource timing secondary buffer is not empty, run the following substeps:
        while !self.resource_timing_secondary_buffer.borrow().is_empty() {
            // 1. Let number of excess entries before be resource timing secondary buffer current size.
            let number_of_excess_entries_before = self.resource_timing_secondary_buffer.borrow().len();

            // 2. If can add resource timing entry returns false, then fire an event named resourcetimingbufferfull at the Performance object.
            if !self.can_add_resource_timing_entry() {
                let full_event = CppEvent::create(self.realm, "resourcetimingbufferfull", false, false, false);
                full_event.set_is_trusted(true);
                self.performance(vm).event_target().dispatch_event(&full_event);
            }

            // 3. Run copy secondary buffer.
            self.copy_resource_timing_secondary_buffer(vm);

            // 4. Let number of excess entries after be resource timing secondary buffer current size.
            let number_of_excess_entries_after = self.resource_timing_secondary_buffer.borrow().len();

            // 5. If number of excess entries before is lower than or equals number of excess entries after, then remove
            //    all entries from resource timing secondary buffer, set resource timing secondary buffer current size to
            //    0, and abort these steps.
            if number_of_excess_entries_before <= number_of_excess_entries_after {
                self.resource_timing_secondary_buffer.borrow_mut(vm).clear();
                break;
            }
        }

        // 2. Set resource timing buffer full event pending flag to false.
        self.resource_timing_buffer_full_event_pending.set(false);
    }

    // https://w3c.github.io/resource-timing/#dfn-copy-secondary-buffer
    fn copy_resource_timing_secondary_buffer(&self, vm: &Vm) {
        // 1. While resource timing secondary buffer is not empty and can add resource timing entry returns true,
        //    run the following substeps:
        while !self.resource_timing_secondary_buffer.borrow().is_empty() && self.can_add_resource_timing_entry() {
            // 1. Let entry be the oldest PerformanceResourceTiming in resource timing secondary buffer.
            // 2. Add entry to the end of performance entry buffer.
            // 3. Increment resource timing buffer current size by 1.
            // 4. Remove entry from resource timing secondary buffer.
            // 5. Decrement resource timing secondary buffer current size by 1.
            let mut secondary_buffer = self.resource_timing_secondary_buffer.borrow_mut(vm);
            let entry = secondary_buffer.remove(0);
            self.with_relevant_performance_entry_tuple_mut(vm, &Utf16String::from_utf8("resource"), |tuple| {
                tuple.performance_entry_buffer.push(entry);
            });
        }
    }
}
