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

namespace Web::HTML {

// https://html.spec.whatwg.org/#timerhandler
using TimerHandler = Variant<JS::NonnullGCPtr<WebIDL::CallbackType>, String>;

// https://html.spec.whatwg.org/multipage/webappapis.html#windoworworkerglobalscope
class WindowOrWorkerGlobalScopeMixin {
public:
    virtual ~WindowOrWorkerGlobalScopeMixin();

    virtual Bindings::PlatformObject& this_impl() = 0;
    virtual Bindings::PlatformObject const& this_impl() const = 0;

    // JS API functions
    WebIDL::ExceptionOr<String> origin() const;
    bool is_secure_context() const;
    bool cross_origin_isolated() const;
    WebIDL::ExceptionOr<String> btoa(String const& data) const;
    WebIDL::ExceptionOr<String> atob(String const& data) const;
    void queue_microtask(WebIDL::CallbackType&);
    JS::NonnullGCPtr<JS::Promise> create_image_bitmap(ImageBitmapSource image, Optional<ImageBitmapOptions> options = {}) const;
    JS::NonnullGCPtr<JS::Promise> create_image_bitmap(ImageBitmapSource image, WebIDL::Long sx, WebIDL::Long sy, WebIDL::Long sw, WebIDL::Long sh, Optional<ImageBitmapOptions> options = {}) const;
    WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Value, StructuredSerializeOptions const&) const;
    JS::NonnullGCPtr<JS::Promise> fetch(Fetch::RequestInfo const&, Fetch::RequestInit const&) const;

    i32 set_timeout(TimerHandler, i32 timeout, JS::MarkedVector<JS::Value> arguments);
    i32 set_interval(TimerHandler, i32 timeout, JS::MarkedVector<JS::Value> arguments);
    void clear_timeout(i32);
    void clear_interval(i32);
    void clear_map_of_active_timers();

    PerformanceTimeline::PerformanceEntryTuple& relevant_performance_entry_tuple(FlyString const& entry_type);
    void queue_performance_entry(JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry> new_entry);
    void clear_performance_entry_buffer(Badge<HighResolutionTime::Performance>, FlyString const& entry_type);
    void remove_entries_from_performance_entry_buffer(Badge<HighResolutionTime::Performance>, FlyString const& entry_type, String entry_name);

    ErrorOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> filter_buffer_map_by_name_and_type(Optional<String> name, Optional<String> type) const;

    void register_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, JS::NonnullGCPtr<PerformanceTimeline::PerformanceObserver>);
    void unregister_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, JS::NonnullGCPtr<PerformanceTimeline::PerformanceObserver>);
    bool has_registered_performance_observer(JS::NonnullGCPtr<PerformanceTimeline::PerformanceObserver>);

    void queue_the_performance_observer_task();

    void register_event_source(Badge<EventSource>, JS::NonnullGCPtr<EventSource>);
    void unregister_event_source(Badge<EventSource>, JS::NonnullGCPtr<EventSource>);
    void forcibly_close_all_event_sources();

    void run_steps_after_a_timeout(i32 timeout, Function<void()> completion_step);

    [[nodiscard]] JS::NonnullGCPtr<HighResolutionTime::Performance> performance();

    JS::NonnullGCPtr<JS::Object> supported_entry_types() const;

    JS::NonnullGCPtr<IndexedDB::IDBFactory> indexed_db();

    void report_error(JS::Value e);
    void report_an_exception(JS::Value const& e);

    [[nodiscard]] JS::NonnullGCPtr<Crypto::Crypto> crypto();

    void push_onto_outstanding_rejected_promises_weak_set(JS::Promise*);

    // Returns true if removed, false otherwise.
    bool remove_from_outstanding_rejected_promises_weak_set(JS::Promise*);

    void push_onto_about_to_be_notified_rejected_promises_list(JS::NonnullGCPtr<JS::Promise>);

    // Returns true if removed, false otherwise.
    bool remove_from_about_to_be_notified_rejected_promises_list(JS::NonnullGCPtr<JS::Promise>);

    void notify_about_rejected_promises(Badge<EventLoop>);

protected:
    void initialize(JS::Realm&);
    void visit_edges(JS::Cell::Visitor&);
    void finalize();

private:
    enum class Repeat {
        Yes,
        No,
    };
    i32 run_timer_initialization_steps(TimerHandler handler, i32 timeout, JS::MarkedVector<JS::Value> arguments, Repeat repeat, Optional<i32> previous_id = {});
    void run_steps_after_a_timeout_impl(i32 timeout, Function<void()> completion_step, Optional<i32> timer_key = {});

    JS::NonnullGCPtr<JS::Promise> create_image_bitmap_impl(ImageBitmapSource& image, Optional<WebIDL::Long> sx, Optional<WebIDL::Long> sy, Optional<WebIDL::Long> sw, Optional<WebIDL::Long> sh, Optional<ImageBitmapOptions>& options) const;

    IDAllocator m_timer_id_allocator;
    HashMap<int, JS::NonnullGCPtr<Timer>> m_timers;

    // https://www.w3.org/TR/performance-timeline/#performance-timeline
    // Each global object has:
    // - a performance observer task queued flag
    bool m_performance_observer_task_queued { false };

    // - a list of registered performance observer objects that is initially empty
    OrderedHashTable<JS::NonnullGCPtr<PerformanceTimeline::PerformanceObserver>> m_registered_performance_observer_objects;

    // https://www.w3.org/TR/performance-timeline/#dfn-performance-entry-buffer-map
    // a performance entry buffer map map, keyed on a DOMString, representing the entry type to which the buffer belongs. The map's value is the following tuple:
    // NOTE: See the PerformanceEntryTuple struct above for the map's value tuple.
    OrderedHashMap<FlyString, PerformanceTimeline::PerformanceEntryTuple> m_performance_entry_buffer_map;

    HashTable<JS::NonnullGCPtr<EventSource>> m_registered_event_sources;

    JS::GCPtr<HighResolutionTime::Performance> m_performance;

    JS::GCPtr<IndexedDB::IDBFactory> m_indexed_db;

    mutable JS::GCPtr<JS::Object> m_supported_entry_types_array;

    JS::GCPtr<Crypto::Crypto> m_crypto;

    bool m_error_reporting_mode { false };

    // https://html.spec.whatwg.org/multipage/webappapis.html#about-to-be-notified-rejected-promises-list
    Vector<JS::Handle<JS::Promise>> m_about_to_be_notified_rejected_promises_list;

    // https://html.spec.whatwg.org/multipage/webappapis.html#outstanding-rejected-promises-weak-set
    // The outstanding rejected promises weak set must not create strong references to any of its members, and implementations are free to limit its size, e.g. by removing old entries from it when new ones are added.
    Vector<JS::GCPtr<JS::Promise>> m_outstanding_rejected_promises_weak_set;
};

}
