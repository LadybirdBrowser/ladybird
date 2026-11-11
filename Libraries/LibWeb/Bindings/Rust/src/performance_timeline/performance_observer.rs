/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceObserver, https://w3c.github.io/performance-timeline/#the-performanceobserver-interface

use core::cell::{Cell, RefCell};

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, MarkedVec, PropertyKey, Realm, ThrowCompletionOr, Trace, Value, Vm};

use crate::bindings::callback::CallbackFunction;
use crate::bindings::conversions::{CppInterface, ToJs, frozen_array_to_js, sequence_to_js};
use crate::bindings::interface::{Ref, Wrappable, allocate, create_dictionary_object, realm_is_window, wrap};
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::performance_timeline::entry_types::supported_entry_types;
use crate::performance_timeline::performance_entry::{CppPerformanceEntry, wrap_entries};
use crate::performance_timeline::performance_observer_entry_list::PerformanceObserverEntryList;
use crate::performance_timeline::performance_timeline::PerformanceTimeline;

// https://w3c.github.io/performance-timeline/#dom-performanceobserverinit
#[derive(Clone, Default)]
pub(crate) struct PerformanceObserverInit {
    pub(crate) entry_types: Option<Vec<Utf16String>>,
    pub(crate) r#type: Option<Utf16String>,
    pub(crate) buffered: Option<bool>,
    // https://w3c.github.io/event-timing/#sec-modifications-perf-timeline
    pub(crate) duration_threshold: Option<f64>,
}

// https://w3c.github.io/performance-timeline/#dfn-observer-type
#[derive(Clone, Copy, PartialEq, Eq)]
enum ObserverType {
    Undefined,
    Single,
    Multiple,
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceObserver {
    realm: Gc<Realm>,

    // https://w3c.github.io/performance-timeline/#dfn-observer-callback
    // A PerformanceObserverCallback observer callback set on creation.
    callback: CallbackFunction,

    // https://w3c.github.io/performance-timeline/#dfn-observer-buffer
    // A PerformanceEntryList object called the observer buffer that is initially empty.
    observer_buffer: GcRefCell<Vec<CppPerformanceEntry>>,

    // https://w3c.github.io/performance-timeline/#dfn-observer-type
    // A DOMString observer type which is initially "undefined".
    #[gc(untraced)]
    observer_type: Cell<ObserverType>,

    // https://w3c.github.io/performance-timeline/#dfn-requires-dropped-entries
    // A boolean requires dropped entries which is initially set to false.
    requires_dropped_entries: Cell<bool>,

    // https://w3c.github.io/performance-timeline/#dfn-options-list
    // A registered performance observer is a struct consisting of an observer member (a PerformanceObserver object)
    // and an options list member (a list of PerformanceObserverInit objects).
    // NOTE: This doesn't use a separate struct as methods such as disconnect() assume it can access an options list from `this`: a PerformanceObserver.
    #[gc(untraced)]
    options_list: RefCell<Vec<PerformanceObserverInit>>,
}

impl Wrappable for PerformanceObserver {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

fn is_supported_entry_type(global_is_window: bool, r#type: &Utf16String) -> bool {
    supported_entry_types(global_is_window).any(|supported| *r#type == Utf16String::from_utf8(supported.entry_type))
}

impl PerformanceObserver {
    // https://w3c.github.io/performance-timeline/#dom-performanceobserver-constructor
    pub(crate) fn construct(cx: &mut CallContext, callback: CallbackFunction) -> WebIdlResult<Ref<Self>> {
        Ok(allocate(cx.vm(), |_| Self {
            realm: cx.realm(),
            callback,
            observer_buffer: GcRefCell::default(),
            observer_type: Cell::new(ObserverType::Undefined),
            requires_dropped_entries: Cell::new(false),
            options_list: RefCell::default(),
        }))
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceobserver-observe
    pub(crate) fn observe(
        this: &Ref<Self>,
        cx: &mut CallContext,
        options: PerformanceObserverInit,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let relevantGlobal be this's relevant global object.
        let relevant_global = PerformanceTimeline::of(vm, this.realm);

        // 2. If options's entryTypes and type members are both omitted, then throw a "TypeError".
        if options.entry_types.is_none() && options.r#type.is_none() {
            return Err(Exception::type_error("Must specify one of entryTypes or type"));
        }

        // 3. If options's entryTypes is present and any other member is also present, then throw a "TypeError".
        if options.entry_types.is_some() && (options.r#type.is_some() || options.buffered.is_some()) {
            return Err(Exception::type_error(
                "Cannot specify type or buffered if entryTypes is specified",
            ));
        }

        // 4. Update or check this's observer type by running these steps:
        match this.observer_type.get() {
            // 1. If this's observer type is "undefined":
            ObserverType::Undefined => {
                // 1. If options's entryTypes member is present, then set this's observer type to "multiple".
                if options.entry_types.is_some() {
                    this.observer_type.set(ObserverType::Multiple);
                }

                // 2. If options's type member is present, then set this's observer type to "single".
                if options.r#type.is_some() {
                    this.observer_type.set(ObserverType::Single);
                }
            }
            // 2. If this's observer type is "single" and options's entryTypes member is present, then throw an
            //    "InvalidModificationError".
            ObserverType::Single => {
                if options.entry_types.is_some() {
                    return Err(Exception::Dom(
                        "InvalidModificationError",
                        Utf16String::from_utf8(
                            "Cannot change a PerformanceObserver from observing a single type to observing multiple types",
                        ),
                    ));
                }
            }
            // 3. If this's observer type is "multiple" and options's type member is present, then throw an
            //    "InvalidModificationError".
            ObserverType::Multiple => {
                if options.r#type.is_some() {
                    return Err(Exception::Dom(
                        "InvalidModificationError",
                        Utf16String::from_utf8(
                            "Cannot change a PerformanceObserver from observing multiple types to observing a single type",
                        ),
                    ));
                }
            }
        }

        // 5. Set this's requires dropped entries to true.
        this.requires_dropped_entries.set(true);

        let global_is_window = realm_is_window(this.realm);

        // 6. If this's observer type is "multiple", run the following steps:
        if this.observer_type.get() == ObserverType::Multiple {
            // 1. Let entry types be options's entryTypes sequence.
            let mut options = options;
            let entry_types = options
                .entry_types
                .as_mut()
                .expect("an observer of multiple types observes entryTypes");

            // 2. Remove all types from entry types that are not contained in relevantGlobal's frozen array of supported
            //    entry types. The user agent SHOULD notify developers if entry types is modified. For example, a console
            //    warning listing removed types might be appropriate.
            entry_types.retain(|r#type| is_supported_entry_type(global_is_window, r#type));

            // 3. If the resulting entry types sequence is an empty sequence, abort these steps.
            //    The user agent SHOULD notify developers when the steps are aborted to notify that registration has been
            //    aborted. For example, a console warning might be appropriate.
            if entry_types.is_empty() {
                return Ok(());
            }

            // 4. If the list of registered performance observer objects of relevantGlobal contains a registered
            //    performance observer whose observer is this, replace its options list with a list containing options as
            //    its only item.
            // 5. Otherwise, create and append a registered performance observer object to the list of registered
            //    performance observer objects of relevantGlobal, with observer set to this and options list set to a list
            //    containing options as its only item.
            *this.options_list.borrow_mut() = vec![options];
            relevant_global.register_performance_observer(vm, *this);
            return Ok(());
        }

        // 7. Otherwise, run the following steps:
        // 1. Assert that this's observer type is "single".
        assert!(this.observer_type.get() == ObserverType::Single);

        // 2. If options's type is not contained in the relevantGlobal's frozen array of supported entry types, abort
        //    these steps. The user agent SHOULD notify developers when this happens, for instance via a console warning.
        let r#type = options
            .r#type
            .clone()
            .expect("an observer of a single type observes type");
        if !is_supported_entry_type(global_is_window, &r#type) {
            return Ok(());
        }

        // 3. If the list of registered performance observer objects of relevantGlobal contains a registered performance
        //    observer obs whose observer is this:
        if relevant_global.has_registered_performance_observer(*this) {
            // 1. If obs's options list contains a PerformanceObserverInit item currentOptions whose type is equal to
            //    options's type, replace currentOptions with options in obs's options list.
            let mut options_list = this.options_list.borrow_mut();
            if let Some(current_options) = options_list
                .iter_mut()
                .find(|current_options| current_options.r#type == options.r#type)
            {
                *current_options = options.clone();
            }
            // 2. Otherwise, append options to obs's options list.
            else {
                options_list.push(options.clone());
            }
        }
        // 4. Otherwise, create and append a registered performance observer object to the list of registered performance
        //    observer objects of relevantGlobal, with observer set to the this and options list set to a list containing
        //    options as its only item.
        else {
            *this.options_list.borrow_mut() = vec![options.clone()];
            relevant_global.register_performance_observer(vm, *this);
        }

        // 5. If options's buffered flag is set:
        if options.buffered == Some(true) {
            // 1. Let tuple be the relevant performance entry tuple of options's type and relevantGlobal.
            relevant_global.with_relevant_performance_entry_tuple(&r#type, |tuple| {
                // 2. For each entry in tuple's performance entry buffer:
                let mut observer_buffer = this.observer_buffer.borrow_mut(vm);
                for entry in &tuple.performance_entry_buffer {
                    // 1. If should add entry with entry and options as parameters returns true, append entry to the
                    //    observer buffer.
                    if entry.should_add_entry(Some(&options)) {
                        observer_buffer.push(entry.clone());
                    }
                }
            });

            // 3. Queue the PerformanceObserver task with relevantGlobal as input.
            relevant_global.queue_the_performance_observer_task(vm);
        }

        Ok(())
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceobserver-disconnect
    pub(crate) fn disconnect(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Remove this from the list of registered performance observer objects of relevant global object.
        PerformanceTimeline::of(vm, this.realm).unregister_performance_observer(vm, *this);

        // 2. Empty this's observer buffer.
        this.observer_buffer.borrow_mut(vm).clear();

        // 3. Empty this's options list.
        this.options_list.borrow_mut().clear();
        Ok(())
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceobserver-takerecords
    pub(crate) fn take_records<'vm>(&self, cx: &mut CallContext<'vm>) -> WebIdlResult<MarkedVec<'vm, CppInterface>> {
        // The takeRecords() method must return a copy of this's observer buffer, and also empty this's observer buffer.
        let records = self.take_observer_buffer(cx.vm());
        Ok(wrap_entries(cx, &records))
    }

    pub(crate) fn take_observer_buffer<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, CppPerformanceEntry> {
        let records = MarkedVec::new(vm);
        let mut buffer = self.observer_buffer.borrow_mut(vm);
        records.extend_from_slice(&buffer);
        buffer.clear();
        drop(buffer);
        records
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceobserver-supportedentrytypes
    pub(crate) fn supported_entry_types(cx: &mut CallContext) -> WebIdlResult<Value> {
        // When supportedEntryTypes’s attribute getter is called, run the following steps:
        // 1. Let globalObject be the environment settings object’s global object.
        // 2. Return globalObject’s frozen array of supported entry types.
        let data = realm_data(cx.vm(), cx.realm());
        if let Some(array) = data.performance_observer_supported_entry_types.get() {
            return Ok(Value::from_object(array));
        }
        let global_is_window = realm_is_window(cx.realm());
        let types: Vec<Utf16String> = supported_entry_types(global_is_window)
            .map(|supported| Utf16String::from_utf8(supported.entry_type))
            .collect();
        let array = frozen_array_to_js(cx, sequence_to_js(cx, types, |r#type| r#type.to_js(cx)))?;
        data.performance_observer_supported_entry_types
            .set(Some(array.as_object()));
        Ok(array)
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub(crate) fn requires_dropped_entries(&self) -> bool {
        self.requires_dropped_entries.get()
    }

    pub(crate) fn unset_requires_dropped_entries(&self) {
        self.requires_dropped_entries.set(false);
    }

    pub(crate) fn options_list(&self) -> core::cell::Ref<'_, Vec<PerformanceObserverInit>> {
        self.options_list.borrow()
    }

    pub(crate) fn append_to_observer_buffer(&self, vm: &Vm, entry: CppPerformanceEntry) {
        self.observer_buffer.borrow_mut(vm).push(entry);
    }

    pub(crate) fn invoke_callback(
        this: Ref<Self>,
        vm: &Vm,
        observer_entry_list: Ref<PerformanceObserverEntryList>,
        dropped_entries_count: Option<u64>,
    ) -> ThrowCompletionOr<()> {
        let cx = &CallContext::new(vm, this.realm);
        let callback_options = create_dictionary_object(cx);
        if let Some(dropped_entries_count) = dropped_entries_count {
            callback_options.create_data_property(
                vm,
                &PropertyKey::from_utf8("droppedEntriesCount"),
                dropped_entries_count.to_js(cx)?,
                None,
                None,
            )?;
        }
        let observer = Value::from_object(wrap(cx, this));
        this.callback.invoke(
            Some(observer),
            &[
                Value::from_object(wrap(cx, observer_entry_list)),
                observer,
                Value::from_object(callback_options),
            ],
        )?;
        Ok(())
    }
}
