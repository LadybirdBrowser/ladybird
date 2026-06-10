/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PerformanceMark.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/NavigationTiming/EntryNames.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/UserTiming/PerformanceMark.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::UserTiming {

GC_DEFINE_ALLOCATOR(PerformanceMark);

PerformanceMark::PerformanceMark(String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, Optional<HTML::SerializationRecord> detail)
    : PerformanceTimeline::PerformanceEntry(name, start_time, duration)
    , m_detail(move(detail))
{
}

PerformanceMark::~PerformanceMark() = default;

GC::Ref<PerformanceMark> PerformanceMark::create(String const& mark_name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, Optional<HTML::SerializationRecord> detail)
{
    return GC::Heap::the().allocate<PerformanceMark>(mark_name, start_time, duration, move(detail));
}

// https://w3c.github.io/user-timing/#dfn-performancemark-constructor
WebIDL::ExceptionOr<GC::Ref<PerformanceMark>> PerformanceMark::create_with_options(String const& mark_name, PerformanceMarkOptions const& mark_options, bool is_window_context, HighResolutionTime::DOMHighResTimeStamp default_start_time)
{
    // 1. If the current global object is a Window object and markName uses the same name as a read only attribute in the PerformanceTiming interface, throw a SyntaxError.
    if (is_window_context) {
        bool matched = false;

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _) \
    if (mark_name == NavigationTiming::EntryNames::name)  \
        matched = true;
        ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

        if (matched)
            return WebIDL::SyntaxError::create(Utf16String::formatted("'{}' markName cannot be used in a Window context because it is part of the PerformanceTiming interface", mark_name));
    }

    // NOTE: Step 2 (creating the entry) is done after determining values, as we set the values once during creation and never change them after.

    // 3. Set entry's name attribute to markName.
    auto const& name = mark_name;

    // 4. Set entry's entryType attribute to DOMString "mark".
    // NOTE: Already done via the `entry_type` virtual function.

    // 5. Set entry's startTime attribute as follows:
    HighResolutionTime::DOMHighResTimeStamp start_time { 0.0 };

    // 1. If markOptions's startTime member is present, then:
    if (mark_options.start_time.has_value()) {
        // 1. If markOptions's startTime is negative, throw a TypeError.
        if (mark_options.start_time.value() < 0.0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "startTime cannot be negative"sv };

        // 2. Otherwise, set entry's startTime to the value of markOptions's startTime.
        start_time = mark_options.start_time.value();
    }
    // 2. Otherwise, set it to the value that would be returned by the Performance object's now() method.
    else {
        start_time = default_start_time;
    }

    // 6. Set entry's duration attribute to 0.
    constexpr HighResolutionTime::DOMHighResTimeStamp duration = 0.0;

    // 2. Create a new PerformanceMark object (entry).
    return PerformanceMark::create(name, start_time, duration, mark_options.detail);
}

WebIDL::ExceptionOr<PerformanceMarkOptions> PerformanceMark::options_from_bindings(JS::Realm& realm, Bindings::PerformanceMarkOptions const& options)
{
    Optional<HTML::SerializationRecord> detail;
    if (options.detail.has_value() && !options.detail->is_null())
        detail = TRY(HTML::structured_serialize(realm, *options.detail));

    return PerformanceMarkOptions {
        .detail = move(detail),
        .start_time = options.start_time,
    };
}

WebIDL::ExceptionOr<GC::Ref<PerformanceMark>> PerformanceMark::create_for_constructor(JS::Realm& realm, String const& mark_name, Bindings::PerformanceMarkOptions const& mark_options)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(realm.global_object());
    VERIFY(global_scope);

    auto is_window_context = is<HTML::Window>(global_scope->this_impl());
    auto default_start_time = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*global_scope));
    return create_with_options(mark_name, TRY(options_from_bindings(realm, mark_options)), is_window_context, default_start_time);
}

FlyString const& PerformanceMark::entry_type() const
{
    return PerformanceTimeline::EntryTypes::mark;
}

WebIDL::ExceptionOr<JS::Value> PerformanceMark::detail(JS::Realm& realm) const
{
    if (!m_detail.has_value())
        return JS::js_null();

    return HTML::structured_deserialize(realm.vm(), m_detail.value(), realm);
}

void PerformanceMark::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
