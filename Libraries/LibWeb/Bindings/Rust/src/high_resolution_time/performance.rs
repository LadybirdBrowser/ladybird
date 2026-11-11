/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Performance, https://w3c.github.io/hr-time/#sec-performance

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, MarkedVec, Realm, Trace, Value, Vm, utf16_formatted};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::global_object::{CppWindow, window_from_global_object};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::html::window_or_worker_global_scope::structured_clone;
use crate::performance_timeline::performance_entry::wrap_entries;
use crate::performance_timeline::performance_timeline::{PerformanceTimeline, PerformanceTimelineRef};
use crate::user_timing::performance_mark::{PerformanceMarkOptions, run_the_performance_mark_constructor};
use crate::user_timing::performance_measure::{
    DOMStringOrDouble, PerformanceMeasureOptions, create_performance_measure,
};

unsafe extern "C" {
    fn web_high_resolution_time_current_high_resolution_time(global_object: *mut c_void) -> f64;
    fn web_high_resolution_time_get_time_origin_timestamp(global_object: *mut c_void) -> f64;
    fn web_navigation_timing_performance_timing_create(window: *mut c_void) -> *mut c_void;
    fn web_navigation_timing_performance_navigation_create(window: *mut c_void) -> *mut c_void;
    fn web_navigation_timing_is_performance_timing_attribute(name: usize) -> bool;
    fn web_navigation_timing_performance_timing_attribute(timing: *mut c_void, name: usize) -> u64;
    fn web_event_timing_event_counts(window: *mut c_void) -> *mut c_void;
    fn web_event_timing_interaction_count(window: *mut c_void) -> u64;
}

pub(crate) enum DOMStringOrPerformanceMeasureOptions {
    DOMString(Utf16String),
    PerformanceMeasureOptions(PerformanceMeasureOptions),
}

// https://w3c.github.io/hr-time/#sec-performance
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Performance {
    event_target: CppEventTarget,
    realm: Gc<Realm>,
    timing: GcRefCell<Option<CppCell>>,
    navigation: GcRefCell<Option<CppCell>>,
}

impl Wrappable for Performance {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for Performance {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

fn is_performance_timing_attribute(name: &Utf16String) -> bool {
    // SAFETY: The name is lent for the call.
    unsafe { web_navigation_timing_is_performance_timing_attribute(name.raw_identity()) }
}

impl Performance {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        let performance = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            realm,
            timing: GcRefCell::default(),
            navigation: GcRefCell::default(),
        });
        performance.event_target.set_owner(performance);
        performance
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    fn performance_timeline(&self, vm: &Vm) -> PerformanceTimelineRef {
        PerformanceTimeline::of(vm, self.realm)
    }

    fn window(&self) -> Option<CppWindow> {
        window_from_global_object(self.realm.global_object())
    }

    fn timing_cell(&self, vm: &Vm) -> CppCell {
        if let Some(timing) = self.timing.borrow().as_ref() {
            return timing.clone();
        }
        let window = self.window().expect("only a Window has a PerformanceTiming");
        // SAFETY: The Window is live, and LibWeb returns a new, live PerformanceTiming.
        let timing = unsafe { CppCell::from_raw(web_navigation_timing_performance_timing_create(window.as_ptr())) };
        *self.timing.borrow_mut(vm) = Some(timing.clone());
        timing
    }

    // https://w3c.github.io/navigation-timing/#dom-performance-timing
    pub(crate) fn timing(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(wrap_cpp_cell(cx, &self.timing_cell(cx.vm())))
    }

    // https://w3c.github.io/navigation-timing/#dom-performance-navigation
    pub(crate) fn navigation(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        if let Some(navigation) = self.navigation.borrow().as_ref() {
            return Ok(wrap_cpp_cell(cx, navigation));
        }
        let window = self.window().expect("only a Window has a PerformanceNavigation");
        // SAFETY: The Window is live, and LibWeb returns a new, live PerformanceNavigation.
        let navigation =
            unsafe { CppCell::from_raw(web_navigation_timing_performance_navigation_create(window.as_ptr())) };
        *self.navigation.borrow_mut(cx.vm()) = Some(navigation.clone());
        Ok(wrap_cpp_cell(cx, &navigation))
    }

    // https://w3c.github.io/event-timing/#dom-performance-eventcounts
    pub(crate) fn event_counts(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // The eventCounts attribute's getter returns this's relevant global object's eventCounts.
        let window = self.window().expect("only a Window has eventCounts");
        // SAFETY: The Window is live, and LibWeb returns its live EventCounts.
        let event_counts = unsafe { CppCell::from_raw(web_event_timing_event_counts(window.as_ptr())) };
        Ok(wrap_cpp_cell(cx, &event_counts))
    }

    // https://w3c.github.io/event-timing/#dom-performance-interactioncount
    pub(crate) fn interaction_count(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // The interactionCount attribute's getter returns this's relevant global object's interactionCount.
        let window = self.window().expect("only a Window has an interactionCount");
        // SAFETY: The Window is live.
        Ok(unsafe { web_event_timing_interaction_count(window.as_ptr()) })
    }

    // https://w3c.github.io/hr-time/#timeorigin-attribute
    pub(crate) fn time_origin(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The timeOrigin attribute MUST return the number of milliseconds in the duration returned by get time origin timestamp for the relevant global object of this.
        // SAFETY: The global object is live.
        Ok(unsafe { web_high_resolution_time_get_time_origin_timestamp(self.realm.global_object().as_ptr().cast()) })
    }

    // https://w3c.github.io/hr-time/#now-method
    pub(crate) fn now(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The now() method MUST return the number of milliseconds in the current high resolution time given this's relevant global object (a duration).
        // SAFETY: The global object is live.
        Ok(
            unsafe {
                web_high_resolution_time_current_high_resolution_time(self.realm.global_object().as_ptr().cast())
            },
        )
    }

    // https://w3c.github.io/user-timing/#mark-method
    pub(crate) fn mark(
        &self,
        cx: &mut CallContext,
        mark_name: Utf16String,
        mark_options: PerformanceMarkOptions,
    ) -> WebIdlResult<CppInterface> {
        let vm = cx.vm();

        // 1. Run the PerformanceMark constructor and let entry be the newly created object.
        let entry = run_the_performance_mark_constructor(self.realm, &mark_name, &mark_options)?;

        // 2. Queue entry.
        let timeline = self.performance_timeline(vm);
        timeline.queue_performance_entry(vm, &entry);

        // 3. Add entry to the performance entry buffer.
        timeline.add_performance_entry(vm, &entry, false);

        // 4. Return entry.
        Ok(entry.wrap(cx))
    }

    pub(crate) fn clear_marks(&self, cx: &mut CallContext, mark_name: Option<Utf16String>) -> WebIdlResult<()> {
        let vm = cx.vm();
        let timeline = self.performance_timeline(vm);
        let entry_type = Utf16String::from_utf8("mark");

        // 1. If markName is omitted, remove all PerformanceMark objects from the performance entry buffer.
        let Some(mark_name) = mark_name else {
            timeline.clear_performance_entry_buffer(vm, &entry_type);
            return Ok(());
        };

        // 2. Otherwise, remove all PerformanceMark objects listed in the performance entry buffer whose name is markName.
        timeline.remove_entries_from_performance_entry_buffer(vm, &entry_type, &mark_name);

        // 3. Return undefined.
        Ok(())
    }

    fn convert_name_to_timestamp(&self, vm: &Vm, name: &Utf16String) -> WebIdlResult<f64> {
        // 1. If the global object is not a Window object, throw a TypeError.
        if self.window().is_none() {
            return Err(Exception::TypeError(utf16_formatted(
                "'{}' is an attribute in the PerformanceTiming interface and thus can only be used in a Window context",
                &[name],
            )));
        }

        // 2. If name is navigationStart, return 0.
        if *name == Utf16String::from_utf8("navigationStart") {
            return Ok(0.0);
        }

        let timing = self.timing_cell(vm);

        // 3. Let startTime be the value of navigationStart in the PerformanceTiming interface.
        // SAFETY: The PerformanceTiming is live, and the name is lent for the call.
        let start_time = unsafe {
            web_navigation_timing_performance_timing_attribute(
                timing.as_ptr(),
                Utf16String::from_utf8("navigationStart").raw_identity(),
            )
        };

        // 4. Let endTime be the value of name in the PerformanceTiming interface.
        // SAFETY: The PerformanceTiming is live, and the name is lent for the call.
        let end_time =
            unsafe { web_navigation_timing_performance_timing_attribute(timing.as_ptr(), name.raw_identity()) };

        // 5. If endTime is 0, throw an InvalidAccessError.
        if end_time == 0 {
            return Err(Exception::Dom(
                "InvalidAccessError",
                utf16_formatted(
                    "The '{}' entry in the PerformanceTiming interface is equal to 0, meaning it hasn't happened yet",
                    &[name],
                ),
            ));
        }

        // 6. Return result of subtracting startTime from endTime.
        Ok(end_time.wrapping_sub(start_time) as f64)
    }

    // https://w3c.github.io/user-timing/#dfn-convert-a-mark-to-a-timestamp
    fn convert_mark_to_timestamp(&self, cx: &CallContext, mark: &DOMStringOrDouble) -> WebIdlResult<f64> {
        let mark_time_stamp = match mark {
            DOMStringOrDouble::DOMString(mark_string) => {
                // 1. If mark is a DOMString and it has the same name as a read only attribute in the PerformanceTiming interface, let end
                //    time be the value returned by running the convert a name to a timestamp algorithm with name set to the value of mark.
                if is_performance_timing_attribute(mark_string) {
                    return self.convert_name_to_timestamp(cx.vm(), mark_string);
                }

                // 2. Otherwise, if mark is a DOMString, let end time be the value of the startTime attribute from the most recent occurrence
                //    of a PerformanceMark object in the performance entry buffer whose name is mark. If no matching entry is found, throw a
                //    SyntaxError.
                let start_time = self
                    .performance_timeline(cx.vm())
                    .with_relevant_performance_entry_tuple(&Utf16String::from_utf8("mark"), |tuple| {
                        tuple
                            .performance_entry_buffer
                            .iter()
                            .rev()
                            .find(|entry| entry.name() == *mark_string)
                            .map(|entry| entry.start_time())
                    });

                return start_time.ok_or_else(|| {
                    Exception::syntax_error(utf16_formatted(
                        "No PerformanceMark object with name '{}' found in the performance timeline",
                        &[mark_string],
                    ))
                });
            }
            // 3. Otherwise, if mark is a DOMHighResTimeStamp:
            DOMStringOrDouble::Double(mark_time_stamp) => *mark_time_stamp,
        };

        // 1. If mark is negative, throw a TypeError.
        if mark_time_stamp < 0.0 {
            return Err(Exception::type_error(
                "Cannot have negative time values in PerformanceMark",
            ));
        }

        // 2. Otherwise, let end time be mark.
        Ok(mark_time_stamp)
    }

    // https://w3c.github.io/user-timing/#dom-performance-measure
    pub(crate) fn measure(
        &self,
        cx: &mut CallContext,
        measure_name: Utf16String,
        start_or_measure_options: DOMStringOrPerformanceMeasureOptions,
        end_mark: Option<Utf16String>,
    ) -> WebIdlResult<CppInterface> {
        let vm = cx.vm();

        // 1. If startOrMeasureOptions is a PerformanceMeasureOptions object and at least one of start, end, duration, and detail
        //    are present, run the following checks:
        let options = match &start_or_measure_options {
            DOMStringOrPerformanceMeasureOptions::PerformanceMeasureOptions(options) => Some(options),
            DOMStringOrPerformanceMeasureOptions::DOMString(_) => None,
        };
        if let Some(options) = options
            && (options.start.is_some()
                || options.end.is_some()
                || options.duration.is_some()
                || options.detail.is_some())
        {
            // 1. If endMark is given, throw a TypeError.
            if end_mark.is_some() {
                return Err(Exception::type_error(
                    "Cannot provide PerformanceMeasureOptions and endMark at the same time",
                ));
            }

            // 2. If startOrMeasureOptions's start and end members are both omitted, throw a TypeError.
            if options.start.is_none() && options.end.is_none() {
                return Err(Exception::type_error(
                    "PerformanceMeasureOptions must contain one or both of 'start' and 'end'",
                ));
            }

            // 3. If startOrMeasureOptions's start, duration, and end members are all present, throw a TypeError.
            if options.start.is_some() && options.end.is_some() && options.duration.is_some() {
                return Err(Exception::type_error(
                    "PerformanceMeasureOptions cannot contain 'start', 'duration' and 'end' properties all at once",
                ));
            }
        }

        // 2. Compute end time as follows:
        // 1. If endMark is given, let end time be the value returned by running the convert a mark to a timestamp algorithm passing
        //    in endMark.
        let end_time = if let Some(end_mark) = end_mark {
            self.convert_mark_to_timestamp(cx, &DOMStringOrDouble::DOMString(end_mark))?
        }
        // 2. Otherwise, if startOrMeasureOptions is a PerformanceMeasureOptions object, and if its end member is present, let end
        //    time be the value returned by running the convert a mark to a timestamp algorithm passing in startOrMeasureOptions's end.
        else if let Some(end) = options.and_then(|options| options.end.as_ref()) {
            self.convert_mark_to_timestamp(cx, end)?
        }
        // 3. Otherwise, if startOrMeasureOptions is a PerformanceMeasureOptions object, and if its start and duration members are
        //    both present:
        else if let Some((start, duration)) =
            options.and_then(|options| Some((options.start.as_ref()?, options.duration?)))
        {
            // 1. Let start be the value returned by running the convert a mark to a timestamp algorithm passing in start.
            let start = self.convert_mark_to_timestamp(cx, start)?;

            // 2. Let duration be the value returned by running the convert a mark to a timestamp algorithm passing in duration.
            let duration = self.convert_mark_to_timestamp(cx, &DOMStringOrDouble::Double(duration))?;

            // 3. Let end time be start plus duration.
            start + duration
        }
        // 4. Otherwise, let end time be the value that would be returned by the Performance object's now() method.
        else {
            self.now(cx)?
        };

        // 3. Compute start time as follows:
        // 1. If startOrMeasureOptions is a PerformanceMeasureOptions object, and if its start member is present, let start time be
        //    the value returned by running the convert a mark to a timestamp algorithm passing in startOrMeasureOptions's start.
        let start_time = if let Some(start) = options.and_then(|options| options.start.as_ref()) {
            self.convert_mark_to_timestamp(cx, start)?
        }
        // 2. Otherwise, if startOrMeasureOptions is a PerformanceMeasureOptions object, and if its duration and end members are
        //    both present:
        else if let Some((duration, end)) =
            options.and_then(|options| Some((options.duration?, options.end.as_ref()?)))
        {
            // 1. Let duration be the value returned by running the convert a mark to a timestamp algorithm passing in duration.
            let duration = self.convert_mark_to_timestamp(cx, &DOMStringOrDouble::Double(duration))?;

            // 2. Let end be the value returned by running the convert a mark to a timestamp algorithm passing in end.
            let end = self.convert_mark_to_timestamp(cx, end)?;

            // 3. Let start time be end minus duration.
            end - duration
        }
        // 3. Otherwise, if startOrMeasureOptions is a DOMString, let start time be the value returned by running the convert a mark
        //    to a timestamp algorithm passing in startOrMeasureOptions.
        else if let DOMStringOrPerformanceMeasureOptions::DOMString(start) = &start_or_measure_options {
            self.convert_mark_to_timestamp(cx, &DOMStringOrDouble::DOMString(start.clone()))?
        }
        // 4. Otherwise, let start time be 0.
        else {
            0.0
        };

        // NOTE: Step 4 (creating the entry) is done after determining values, as we set the values once during creation and never
        //       change them after.

        // 5. Set entry's name attribute to measureName.
        // NOTE: Will be done during construction.

        // 6. Set entry's entryType attribute to DOMString "measure".
        // NOTE: Already done via the `entry_type` virtual function.

        // 7. Set entry's startTime attribute to start time.
        // NOTE: Will be done during construction.

        // 8. Set entry's duration attribute to the duration from start time to end time. The resulting duration value MAY be negative.
        let duration = end_time - start_time;

        // 4. Create a new PerformanceMeasure object (entry).
        let mut detail = Value::NULL;
        if let Some(options_detail) = options.and_then(|options| options.detail)
            && !options_detail.is_null()
        {
            detail = structured_clone(&CallContext::new(vm, self.realm), options_detail, &[])?;
        }
        let entry = create_performance_measure(&measure_name, start_time, duration, detail);

        // 10. Queue entry.
        let timeline = self.performance_timeline(vm);
        timeline.queue_performance_entry(vm, &entry);

        // 11. Add entry to the performance entry buffer.
        timeline.add_performance_entry(vm, &entry, false);

        // 12. Return entry.
        Ok(entry.wrap(cx))
    }

    // https://w3c.github.io/user-timing/#dom-performance-clearmeasures
    pub(crate) fn clear_measures(&self, cx: &mut CallContext, measure_name: Option<Utf16String>) -> WebIdlResult<()> {
        let vm = cx.vm();
        let timeline = self.performance_timeline(vm);
        let entry_type = Utf16String::from_utf8("measure");

        // 1. If measureName is omitted, remove all PerformanceMeasure objects in the performance entry buffer.
        let Some(measure_name) = measure_name else {
            timeline.clear_performance_entry_buffer(vm, &entry_type);
            return Ok(());
        };

        // 2. Otherwise remove all PerformanceMeasure objects listed in the performance entry buffer whose name is measureName.
        timeline.remove_entries_from_performance_entry_buffer(vm, &entry_type, &measure_name);

        // 3. Return undefined.
        Ok(())
    }

    // https://w3c.github.io/resource-timing/#dom-performance-clearresourcetimings
    pub(crate) fn clear_resource_timings(&self, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. Remove all PerformanceResourceTiming objects in the performance entry buffer.
        // 2. Set resource timing buffer current size to 0.
        self.performance_timeline(cx.vm())
            .clear_performance_entry_buffer(cx.vm(), &Utf16String::from_utf8("resource"));
        Ok(())
    }

    // https://w3c.github.io/resource-timing/#dom-performance-setresourcetimingbuffersize
    pub(crate) fn set_resource_timing_buffer_size(&self, cx: &mut CallContext, max_size: u32) -> WebIdlResult<()> {
        // 1. Set resource timing buffer size limit to the maxSize parameter. If the maxSize parameter is less than
        //    resource timing buffer current size, no PerformanceResourceTiming objects are to be removed from the
        //    performance entry buffer.
        self.performance_timeline(cx.vm())
            .set_resource_timing_buffer_size_limit(max_size);
        Ok(())
    }

    // https://w3c.github.io/performance-timeline/#getentries-method
    pub(crate) fn get_entries<'vm>(&self, cx: &mut CallContext<'vm>) -> WebIdlResult<MarkedVec<'vm, CppInterface>> {
        // Returns a PerformanceEntryList object returned by the filter buffer map by name and type algorithm with name and
        // type set to null.
        let entries = self
            .performance_timeline(cx.vm())
            .filter_buffer_map_by_name_and_type(cx, None, None);
        Ok(wrap_entries(cx, &entries))
    }

    // https://w3c.github.io/performance-timeline/#dom-performance-getentriesbytype
    pub(crate) fn get_entries_by_type<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
        r#type: Utf16String,
    ) -> WebIdlResult<MarkedVec<'vm, CppInterface>> {
        // Returns a PerformanceEntryList object returned by filter buffer map by name and type algorithm with name set to null,
        // and type set to the method's input type parameter.
        let entries = self
            .performance_timeline(cx.vm())
            .filter_buffer_map_by_name_and_type(cx, None, Some(&r#type));
        Ok(wrap_entries(cx, &entries))
    }

    // https://w3c.github.io/performance-timeline/#dom-performance-getentriesbyname
    pub(crate) fn get_entries_by_name<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
        name: Utf16String,
        r#type: Option<Utf16String>,
    ) -> WebIdlResult<MarkedVec<'vm, CppInterface>> {
        // Returns a PerformanceEntryList object returned by filter buffer map by name and type algorithm with name set to the
        // method input name parameter, and type set to null if optional entryType is omitted, or set to the method's input type
        // parameter otherwise.
        let entries =
            self.performance_timeline(cx.vm())
                .filter_buffer_map_by_name_and_type(cx, Some(&name), r#type.as_ref());
        Ok(wrap_entries(cx, &entries))
    }
}
