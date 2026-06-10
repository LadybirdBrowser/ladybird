/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/PerformanceMeasure.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NavigationTiming/EntryNames.h>
#include <LibWeb/NavigationTiming/PerformanceNavigation.h>
#include <LibWeb/NavigationTiming/PerformanceTiming.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/PerformanceTimeline/EventNames.h>

namespace Web::HighResolutionTime {

GC_DEFINE_ALLOCATOR(Performance);

Performance::Performance(GC::Ref<DOM::EventTarget> relevant_global_object)
    : DOM::EventTarget()
    , m_global_object(relevant_global_object)
{
}

Performance::~Performance() = default;

void Performance::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_navigation);
    visitor.visit(m_timing);
    visitor.visit(m_global_object);
}

GC::Ptr<NavigationTiming::PerformanceTiming> Performance::timing()
{
    if (!m_timing) {
        auto* window = HTML::window_from_global_object(relevant_global_object());
        VERIFY(window);
        m_timing = NavigationTiming::PerformanceTiming::create(*window);
    }
    return m_timing;
}

GC::Ptr<NavigationTiming::PerformanceNavigation> Performance::navigation()
{
    if (!m_navigation) {
        // FIXME: actually determine values for these
        u16 type = 0;
        u16 redirect_count = 0;

        m_navigation = NavigationTiming::PerformanceNavigation::create(type, redirect_count);
    }
    return m_navigation;
}

// https://w3c.github.io/hr-time/#timeorigin-attribute
double Performance::time_origin() const
{
    // The timeOrigin attribute MUST return the number of milliseconds in the duration returned by get time origin timestamp for the relevant global object of this.
    return get_time_origin_timestamp(relevant_global_object());
}

// https://w3c.github.io/hr-time/#now-method
double Performance::now() const
{
    // The now() method MUST return the number of milliseconds in the current high resolution time given this's relevant global object (a duration).
    return current_high_resolution_time(relevant_global_object());
}

// https://w3c.github.io/user-timing/#mark-method
WebIDL::ExceptionOr<GC::Ref<UserTiming::PerformanceMark>> Performance::mark(String const& mark_name, UserTiming::PerformanceMarkOptions const& mark_options)
{
    // 1. Run the PerformanceMark constructor and let entry be the newly created object.
    auto entry = TRY(UserTiming::PerformanceMark::create_with_options(mark_name, mark_options, is<HTML::Window>(window_or_worker().this_impl()), now()));

    // 2. Queue entry.
    window_or_worker().queue_performance_entry(entry);

    // 3. Add entry to the performance entry buffer.
    window_or_worker().add_performance_entry(entry);

    // 4. Return entry.
    return entry;
}

WebIDL::ExceptionOr<GC::Ref<UserTiming::PerformanceMark>> Performance::mark(JS::Realm& realm, String const& mark_name, Bindings::PerformanceMarkOptions const& mark_options)
{
    return mark(mark_name, TRY(UserTiming::PerformanceMark::options_from_bindings(realm, mark_options)));
}

void Performance::clear_marks(Optional<String> mark_name)
{
    // 1. If markName is omitted, remove all PerformanceMark objects from the performance entry buffer.
    if (!mark_name.has_value()) {
        window_or_worker().clear_performance_entry_buffer({}, PerformanceTimeline::EntryTypes::mark);
        return;
    }

    // 2. Otherwise, remove all PerformanceMark objects listed in the performance entry buffer whose name is markName.
    window_or_worker().remove_entries_from_performance_entry_buffer({}, PerformanceTimeline::EntryTypes::mark, mark_name.value());

    // 3. Return undefined.
}

WebIDL::ExceptionOr<HighResolutionTime::DOMHighResTimeStamp> Performance::convert_name_to_timestamp(String const& name)
{
    // 1. If the global object is not a Window object, throw a TypeError.
    if (!HTML::window_from_global_object(relevant_global_object()))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, TRY_OR_THROW_OOM(JS::VM::the(), String::formatted("'{}' is an attribute in the PerformanceTiming interface and thus can only be used in a Window context", name)) };

    // 2. If name is navigationStart, return 0.
    if (name == NavigationTiming::EntryNames::navigationStart)
        return 0.0;

    auto timing_interface = timing();
    VERIFY(timing_interface);

    // 3. Let startTime be the value of navigationStart in the PerformanceTiming interface.
    auto start_time = timing_interface->navigation_start();

    // 4. Let endTime be the value of name in the PerformanceTiming interface.
    u64 end_time { 0 };

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(camel_case_name, snake_case_name) \
    if (name == NavigationTiming::EntryNames::camel_case_name)                     \
        end_time = timing_interface->snake_case_name();
    ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

    // 5. If endTime is 0, throw an InvalidAccessError.
    if (end_time == 0)
        return WebIDL::InvalidAccessError::create(Utf16String::formatted("The '{}' entry in the PerformanceTiming interface is equal to 0, meaning it hasn't happened yet", name));

    // 6. Return result of subtracting startTime from endTime.
    return static_cast<HighResolutionTime::DOMHighResTimeStamp>(end_time - start_time);
}

// https://w3c.github.io/user-timing/#dfn-convert-a-mark-to-a-timestamp
WebIDL::ExceptionOr<HighResolutionTime::DOMHighResTimeStamp> Performance::convert_mark_to_timestamp(Variant<String, HighResolutionTime::DOMHighResTimeStamp> mark)
{
    if (mark.has<String>()) {
        auto const& mark_string = mark.get<String>();

        // 1. If mark is a DOMString and it has the same name as a read only attribute in the PerformanceTiming interface, let end
        //    time be the value returned by running the convert a name to a timestamp algorithm with name set to the value of mark.
#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _)  \
    if (mark_string == NavigationTiming::EntryNames::name) \
        return convert_name_to_timestamp(mark_string);
        ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

        // 2. Otherwise, if mark is a DOMString, let end time be the value of the startTime attribute from the most recent occurrence
        //    of a PerformanceMark object in the performance entry buffer whose name is mark. If no matching entry is found, throw a
        //    SyntaxError.
        auto& tuple = window_or_worker().relevant_performance_entry_tuple(PerformanceTimeline::EntryTypes::mark);
        auto& performance_entry_buffer = tuple.performance_entry_buffer;

        auto maybe_entry = performance_entry_buffer.last_matching([&mark_string](GC::Root<PerformanceTimeline::PerformanceEntry> const& entry) {
            return entry->name() == mark_string;
        });

        if (!maybe_entry.has_value())
            return WebIDL::SyntaxError::create(Utf16String::formatted("No PerformanceMark object with name '{}' found in the performance timeline", mark_string));

        return maybe_entry.value()->start_time();
    }

    // 3. Otherwise, if mark is a DOMHighResTimeStamp:
    auto mark_time_stamp = mark.get<HighResolutionTime::DOMHighResTimeStamp>();

    // 1. If mark is negative, throw a TypeError.
    if (mark_time_stamp < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot have negative time values in PerformanceMark"sv };

    // 2. Otherwise, let end time be mark.
    return mark_time_stamp;
}

static WebIDL::ExceptionOr<PerformanceMeasureOptions> performance_measure_options_from_bindings(JS::Realm& realm, Bindings::PerformanceMeasureOptions const& options)
{
    Optional<HTML::SerializationRecord> detail;
    if (options.detail.has_value())
        detail = TRY(HTML::structured_serialize(realm, *options.detail));

    return PerformanceMeasureOptions {
        .detail = move(detail),
        .duration = options.duration,
        .end = options.end,
        .start = options.start,
    };
}

// https://w3c.github.io/user-timing/#dom-performance-measure
WebIDL::ExceptionOr<GC::Ref<UserTiming::PerformanceMeasure>> Performance::measure(String const& measure_name, Variant<String, PerformanceMeasureOptions> const& start_or_measure_options, Optional<String> end_mark)
{
    // 1. If startOrMeasureOptions is a PerformanceMeasureOptions object and at least one of start, end, duration, and detail
    //    are present, run the following checks:
    auto const* options = start_or_measure_options.get_pointer<PerformanceMeasureOptions>();
    if (options
        && (options->start.has_value()
            || options->end.has_value()
            || options->duration.has_value()
            || options->detail.has_value())) {
        // 1. If endMark is given, throw a TypeError.
        if (end_mark.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot provide PerformanceMeasureOptions and endMark at the same time"sv };

        // 2. If startOrMeasureOptions's start and end members are both omitted, throw a TypeError.
        if (!options->start.has_value() && !options->end.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "PerformanceMeasureOptions must contain one or both of 'start' and 'end'"sv };

        // 3. If startOrMeasureOptions's start, duration, and end members are all present, throw a TypeError.
        if (options->start.has_value() && options->end.has_value() && options->duration.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "PerformanceMeasureOptions cannot contain 'start', 'duration' and 'end' properties all at once"sv };
    }

    // 2. Compute end time as follows:
    HighResolutionTime::DOMHighResTimeStamp end_time { 0.0 };

    // 1. If endMark is given, let end time be the value returned by running the convert a mark to a timestamp algorithm passing
    //    in endMark.
    if (end_mark.has_value()) {
        end_time = TRY(convert_mark_to_timestamp(end_mark.value()));
    }
    // 2. Otherwise, if startOrMeasureOptions is a PerformanceMeasureOptions object, and if its end member is present, let end
    //    time be the value returned by running the convert a mark to a timestamp algorithm passing in startOrMeasureOptions's end.
    else if (options && options->end.has_value()) {
        end_time = TRY(convert_mark_to_timestamp(options->end.value()));
    }
    // 3. Otherwise, if startOrMeasureOptions is a PerformanceMeasureOptions object, and if its start and duration members are
    //    both present:
    else if (options && options->start.has_value() && options->duration.has_value()) {
        // 1. Let start be the value returned by running the convert a mark to a timestamp algorithm passing in start.
        auto start = TRY(convert_mark_to_timestamp(options->start.value()));

        // 2. Let duration be the value returned by running the convert a mark to a timestamp algorithm passing in duration.
        auto duration = TRY(convert_mark_to_timestamp(options->duration.value()));

        // 3. Let end time be start plus duration.
        end_time = start + duration;
    }
    // 4. Otherwise, let end time be the value that would be returned by the Performance object's now() method.
    else {
        end_time = now();
    }

    // 3. Compute start time as follows:
    HighResolutionTime::DOMHighResTimeStamp start_time { 0.0 };

    // 1. If startOrMeasureOptions is a PerformanceMeasureOptions object, and if its start member is present, let start time be
    //    the value returned by running the convert a mark to a timestamp algorithm passing in startOrMeasureOptions's start.
    if (options && options->start.has_value()) {
        start_time = TRY(convert_mark_to_timestamp(options->start.value()));
    }
    // 2. Otherwise, if startOrMeasureOptions is a PerformanceMeasureOptions object, and if its duration and end members are
    //    both present:
    else if (options && options->duration.has_value() && options->end.has_value()) {
        // 1. Let duration be the value returned by running the convert a mark to a timestamp algorithm passing in duration.
        auto duration = TRY(convert_mark_to_timestamp(options->duration.value()));

        // 2. Let end be the value returned by running the convert a mark to a timestamp algorithm passing in end.
        auto end = TRY(convert_mark_to_timestamp(options->end.value()));

        // 3. Let start time be end minus duration.
        start_time = end - duration;
    }
    // 3. Otherwise, if startOrMeasureOptions is a DOMString, let start time be the value returned by running the convert a mark
    //    to a timestamp algorithm passing in startOrMeasureOptions.
    else if (start_or_measure_options.has<String>()) {
        start_time = TRY(convert_mark_to_timestamp(start_or_measure_options.get<String>()));
    }
    // 4. Otherwise, let start time be 0.
    else {
        start_time = 0.0;
    }

    // NOTE: Step 4 (creating the entry) is done after determining values, as we set the values once during creation and never
    //       change them after.

    // 5. Set entry's name attribute to measureName.
    // NOTE: Will be done during construction.

    // 6. Set entry's entryType attribute to DOMString "measure".
    // NOTE: Already done via the `entry_type` virtual function.

    // 7. Set entry's startTime attribute to start time.
    // NOTE: Will be done during construction.

    // 8. Set entry's duration attribute to the duration from start time to end time. The resulting duration value MAY be negative.
    auto duration = end_time - start_time;

    // 4. Create a new PerformanceMeasure object (entry).
    auto entry = UserTiming::PerformanceMeasure::create(measure_name, start_time, duration, options ? options->detail : OptionalNone {});

    // 10. Queue entry.
    window_or_worker().queue_performance_entry(entry);

    // 11. Add entry to the performance entry buffer.
    window_or_worker().add_performance_entry(entry);

    // 12. Return entry.
    return entry;
}

WebIDL::ExceptionOr<GC::Ref<UserTiming::PerformanceMeasure>> Performance::measure(JS::Realm& realm, String const& measure_name, Variant<String, Bindings::PerformanceMeasureOptions> const& start_or_measure_options, Optional<String> end_mark)
{
    if (start_or_measure_options.has<String>())
        return measure(measure_name, start_or_measure_options.get<String>(), move(end_mark));

    return measure(measure_name, TRY(performance_measure_options_from_bindings(realm, start_or_measure_options.get<Bindings::PerformanceMeasureOptions>())), move(end_mark));
}

// https://w3c.github.io/user-timing/#dom-performance-clearmeasures
void Performance::clear_measures(Optional<String> measure_name)
{
    // 1. If measureName is omitted, remove all PerformanceMeasure objects in the performance entry buffer.
    if (!measure_name.has_value()) {
        window_or_worker().clear_performance_entry_buffer({}, PerformanceTimeline::EntryTypes::measure);
        return;
    }

    // 2. Otherwise remove all PerformanceMeasure objects listed in the performance entry buffer whose name is measureName.
    window_or_worker().remove_entries_from_performance_entry_buffer({}, PerformanceTimeline::EntryTypes::measure, measure_name.value());

    // 3. Return undefined.
}

// https://w3c.github.io/resource-timing/#dom-performance-clearresourcetimings
void Performance::clear_resource_timings()
{
    // 1. Remove all PerformanceResourceTiming objects in the performance entry buffer.
    // 2. Set resource timing buffer current size to 0.
    window_or_worker().clear_performance_entry_buffer({}, PerformanceTimeline::EntryTypes::resource);
}

// https://w3c.github.io/resource-timing/#dom-performance-setresourcetimingbuffersize
void Performance::set_resource_timing_buffer_size(u32 max_size)
{
    // 1. Set resource timing buffer size limit to the maxSize parameter. If the maxSize parameter is less than
    //    resource timing buffer current size, no PerformanceResourceTiming objects are to be removed from the
    //    performance entry buffer.
    window_or_worker().set_resource_timing_buffer_size_limit({}, max_size);
}

// https://w3c.github.io/resource-timing/#dom-performance-onresourcetimingbufferfull
void Performance::set_onresourcetimingbufferfull(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(PerformanceTimeline::EventNames::resourcetimingbufferfull, event_handler);
}

// https://w3c.github.io/resource-timing/#dom-performance-onresourcetimingbufferfull
WebIDL::CallbackType* Performance::onresourcetimingbufferfull()
{
    return event_handler_attribute(PerformanceTimeline::EventNames::resourcetimingbufferfull);
}

// https://www.w3.org/TR/performance-timeline/#getentries-method
WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> Performance::get_entries() const
{
    auto& vm = this->vm();

    // Returns a PerformanceEntryList object returned by the filter buffer map by name and type algorithm with name and
    // type set to null.
    return TRY_OR_THROW_OOM(vm, window_or_worker().filter_buffer_map_by_name_and_type(/* name= */ Optional<String> {}, /* type= */ Optional<String> {}));
}

// https://www.w3.org/TR/performance-timeline/#dom-performance-getentriesbytype
WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> Performance::get_entries_by_type(String const& type) const
{
    auto& vm = this->vm();

    // Returns a PerformanceEntryList object returned by filter buffer map by name and type algorithm with name set to null,
    // and type set to the method's input type parameter.
    return TRY_OR_THROW_OOM(vm, window_or_worker().filter_buffer_map_by_name_and_type(/* name= */ Optional<String> {}, type));
}

// https://www.w3.org/TR/performance-timeline/#dom-performance-getentriesbyname
WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> Performance::get_entries_by_name(String const& name, Optional<String> type) const
{
    auto& vm = this->vm();

    // Returns a PerformanceEntryList object returned by filter buffer map by name and type algorithm with name set to the
    // method input name parameter, and type set to null if optional entryType is omitted, or set to the method's input type
    // parameter otherwise.
    return TRY_OR_THROW_OOM(vm, window_or_worker().filter_buffer_map_by_name_and_type(name, type));
}

HTML::WindowOrWorkerGlobalScopeMixin& Performance::window_or_worker()
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(relevant_global_object());
    VERIFY(global_scope);
    return *global_scope;
}

HTML::WindowOrWorkerGlobalScopeMixin const& Performance::window_or_worker() const
{
    return const_cast<Performance*>(this)->window_or_worker();
}

JS::Object& Performance::relevant_global_object() const
{
    return HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

}
