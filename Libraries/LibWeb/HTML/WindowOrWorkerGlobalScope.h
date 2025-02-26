/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/IDAllocator.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntryTuple.h>
#include <LibWeb/WebSockets/WebSocket.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#timerhandler
using TimerHandler = Variant<GC::Ref<WebIDL::CallbackType>, String>;

// https://html.spec.whatwg.org/multipage/webappapis.html#windoworworkerglobalscope
class WindowOrWorkerGlobalScopeMixin {
public:
    virtual ~WindowOrWorkerGlobalScopeMixin();

    virtual DOM::EventTarget& this_impl() = 0;
    virtual DOM::EventTarget const& this_impl() const = 0;

    // JS API functions
    String origin() const;
    bool is_secure_context() const;
    bool cross_origin_isolated() const;
    GC::Ref<WebIDL::Promise> create_image_bitmap(ImageBitmapSource image, Optional<ImageBitmapOptions> options = {}) const;
    GC::Ref<WebIDL::Promise> create_image_bitmap(ImageBitmapSource image, WebIDL::Long sx, WebIDL::Long sy, WebIDL::Long sw, WebIDL::Long sh, Optional<ImageBitmapOptions> options = {}) const;
    GC::Ref<WebIDL::Promise> fetch(Fetch::RequestInfo const&, Fetch::RequestInit const&) const;

    i32 set_timeout(TimerHandler, i32 timeout, GC::RootVector<JS::Value> arguments);
    i32 set_interval(TimerHandler, i32 timeout, GC::RootVector<JS::Value> arguments);
    void clear_timeout(i32);
    void clear_interval(i32);
    void clear_map_of_active_timers();

    enum class CheckIfPerformanceBufferIsFull {
        No,
        Yes,
    };

    PerformanceTimeline::PerformanceEntryTuple& relevant_performance_entry_tuple(FlyString const& entry_type);
    void queue_performance_entry(GC::Ref<PerformanceTimeline::PerformanceEntry> new_entry);
    void add_performance_entry(GC::Ref<PerformanceTimeline::PerformanceEntry> new_entry, CheckIfPerformanceBufferIsFull check_if_performance_buffer_is_full = CheckIfPerformanceBufferIsFull::No);
    void clear_performance_entry_buffer(Badge<HighResolutionTime::Performance>, FlyString const& entry_type);
    void remove_entries_from_performance_entry_buffer(Badge<HighResolutionTime::Performance>, FlyString const& entry_type, String entry_name);

    ErrorOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> filter_buffer_map_by_name_and_type(Optional<String> name, Optional<String> type) const;

    void register_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, GC::Ref<PerformanceTimeline::PerformanceObserver>);
    void unregister_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, GC::Ref<PerformanceTimeline::PerformanceObserver>);
    bool has_registered_performance_observer(GC::Ref<PerformanceTimeline::PerformanceObserver>);

    void queue_the_performance_observer_task();

    void register_event_source(Badge<EventSource>, GC::Ref<EventSource>);
    void unregister_event_source(Badge<EventSource>, GC::Ref<EventSource>);
    void forcibly_close_all_event_sources();

    void register_web_socket(Badge<WebSockets::WebSocket>, GC::Ref<WebSockets::WebSocket>);
    void unregister_web_socket(Badge<WebSockets::WebSocket>, GC::Ref<WebSockets::WebSocket>);

    enum class AffectedAnyWebSockets {
        No,
        Yes,
    };
    AffectedAnyWebSockets make_disappear_all_web_sockets();

    void run_steps_after_a_timeout(i32 timeout, Function<void()> completion_step);

    [[nodiscard]] GC::Ref<HighResolutionTime::Performance> performance();

    GC::Ref<JS::Object> supported_entry_types() const;

    GC::Ref<IndexedDB::IDBFactory> indexed_db();

    void report_error(JS::Value e);

    enum class OmitError {
        Yes,
        No,
    };
    void report_an_exception(JS::Value exception, OmitError = OmitError::No);

    [[nodiscard]] GC::Ref<Crypto::Crypto> crypto();

protected:
    void initialize(JS::Realm&);
    void visit_edges(JS::Cell::Visitor&);
    void finalize();

private:
    enum class Repeat {
        Yes,
        No,
    };
    i32 run_timer_initialization_steps(TimerHandler handler, i32 timeout, GC::RootVector<JS::Value> arguments, Repeat repeat, Optional<i32> previous_id = {});
    void run_steps_after_a_timeout_impl(i32 timeout, Function<void()> completion_step, Optional<i32> timer_key = {});

    GC::Ref<WebIDL::Promise> create_image_bitmap_impl(ImageBitmapSource& image, Optional<WebIDL::Long> sx, Optional<WebIDL::Long> sy, Optional<WebIDL::Long> sw, Optional<WebIDL::Long> sh, Optional<ImageBitmapOptions>& options) const;

    IDAllocator m_timer_id_allocator;
    HashMap<int, GC::Ref<Timer>> m_timers;

    // https://www.w3.org/TR/performance-timeline/#performance-timeline
    // Each global object has:
    // - a performance observer task queued flag
    bool m_performance_observer_task_queued { false };

    // - a list of registered performance observer objects that is initially empty
    OrderedHashTable<GC::Ref<PerformanceTimeline::PerformanceObserver>> m_registered_performance_observer_objects;

    // https://www.w3.org/TR/performance-timeline/#dfn-performance-entry-buffer-map
    // a performance entry buffer map map, keyed on a DOMString, representing the entry type to which the buffer belongs. The map's value is the following tuple:
    // NOTE: See the PerformanceEntryTuple struct above for the map's value tuple.
    OrderedHashMap<FlyString, PerformanceTimeline::PerformanceEntryTuple> m_performance_entry_buffer_map;

    HashTable<GC::Ref<EventSource>> m_registered_event_sources;

    GC::Ptr<HighResolutionTime::Performance> m_performance;

    GC::Ptr<IndexedDB::IDBFactory> m_indexed_db;

    mutable GC::Ptr<JS::Object> m_supported_entry_types_array;

    GC::Ptr<Crypto::Crypto> m_crypto;

    bool m_error_reporting_mode { false };

    WebSockets::WebSocket::List m_registered_web_sockets;
};

}
