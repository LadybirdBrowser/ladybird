/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/IDAllocator.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Variant.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/Scripting/ImportMap.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntryTuple.h>
#include <LibWeb/WebSockets/WebSocket.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#timerhandler
using TimerHandler = Variant<GC::Ref<WebIDL::CallbackType>, Utf16String>;

// https://html.spec.whatwg.org/multipage/webappapis.html#windoworworkerglobalscope
class WEB_API WindowOrWorkerGlobalScopeMixin {
public:
    virtual ~WindowOrWorkerGlobalScopeMixin();

    virtual DOM::EventTarget& this_impl() = 0;
    virtual DOM::EventTarget const& this_impl() const = 0;

    // JS API functions
    Utf16String origin() const;
    bool is_secure_context() const;
    bool cross_origin_isolated() const;

    WebIDL::ExceptionOr<Utf16String> btoa(Utf16View data) const;
    WebIDL::ExceptionOr<Utf16String> atob(Utf16View data) const;

    i32 set_timeout(TimerHandler, i32 timeout, GC::RootVector<JS::Value> arguments);
    i32 set_interval(TimerHandler, i32 timeout, GC::RootVector<JS::Value> arguments);
    void clear_timeout(i32);
    void clear_interval(i32);
    void clear_map_of_active_timers();

    void queue_microtask(WebIDL::CallbackType&);

    void create_image_bitmap(JS::Realm&, ImageBitmapSource image, ImageBitmapOptions options, GC::Ref<WebIDL::Promise>) const;
    void create_image_bitmap(JS::Realm&, ImageBitmapSource image, WebIDL::Long sx, WebIDL::Long sy, WebIDL::Long sw, WebIDL::Long sh, ImageBitmapOptions options, GC::Ref<WebIDL::Promise>) const;

    WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Realm&, JS::Value, Bindings::StructuredSerializeOptions const&);

    enum class CheckIfPerformanceBufferIsFull {
        No,
        Yes,
    };

    PerformanceTimeline::PerformanceEntryTuple& relevant_performance_entry_tuple(Utf16FlyString const& entry_type);
    void queue_performance_entry(GC::Ref<PerformanceTimeline::PerformanceEntry> new_entry);
    void add_performance_entry(GC::Ref<PerformanceTimeline::PerformanceEntry> new_entry, CheckIfPerformanceBufferIsFull check_if_performance_buffer_is_full = CheckIfPerformanceBufferIsFull::No);
    void clear_performance_entry_buffer(Badge<HighResolutionTime::Performance>, Utf16FlyString const& entry_type);
    void remove_entries_from_performance_entry_buffer(Badge<HighResolutionTime::Performance>, Utf16FlyString const& entry_type, Utf16View entry_name);

    ErrorOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> filter_buffer_map_by_name_and_type(Optional<Utf16String> const& name, Optional<Utf16FlyString> type) const;

    void register_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, GC::Ref<PerformanceTimeline::PerformanceObserver>);
    void unregister_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, GC::Ref<PerformanceTimeline::PerformanceObserver>);
    bool has_registered_performance_observer(GC::Ref<PerformanceTimeline::PerformanceObserver>);

    void queue_the_performance_observer_task();

    void set_resource_timing_buffer_size_limit(Badge<HighResolutionTime::Performance>, u32 value) { m_resource_timing_buffer_size_limit = value; }
    void add_resource_timing_entry(Badge<ResourceTiming::PerformanceResourceTiming>, GC::Ref<ResourceTiming::PerformanceResourceTiming> entry);

    void register_event_source(Badge<EventSource>, GC::Ref<EventSource>);
    void unregister_event_source(Badge<EventSource>, GC::Ref<EventSource>);
    void forcibly_close_all_event_sources();

    void close_all_idb_connections();

    void register_web_socket(Badge<WebSockets::WebSocket>, GC::Ref<WebSockets::WebSocket>);
    void unregister_web_socket(Badge<WebSockets::WebSocket>, GC::Ref<WebSockets::WebSocket>);

    enum class AffectedAnyWebSockets {
        No,
        Yes,
    };
    AffectedAnyWebSockets make_disappear_all_web_sockets();

    i32 run_steps_after_a_timeout(i32 timeout, Function<void()> completion_step);

    void report_error(JS::Value e);

    enum class OmitError {
        Yes,
        No,
    };
    void report_an_exception(JS::Value exception, OmitError = OmitError::No);

    GC::Ref<WebIDL::CallbackType> count_queuing_strategy_size_function();
    GC::Ref<WebIDL::CallbackType> byte_length_queuing_strategy_size_function();

    void push_onto_outstanding_rejected_promises_weak_set(GC::Ptr<JS::Promise>);
    bool remove_from_outstanding_rejected_promises_weak_set(GC::Ptr<JS::Promise>);

    void push_onto_about_to_be_notified_rejected_promises_list(GC::Ref<JS::Promise>);
    bool remove_from_about_to_be_notified_rejected_promises_list(GC::Ref<JS::Promise>);

    void notify_about_rejected_promises(Badge<EventLoop>);

    ImportMap& import_map() { return m_import_map; }
    ImportMap const& import_map() const { return m_import_map; }
    void set_import_map(ImportMap const& import_map) { m_import_map = import_map; }

    static void set_experimental_interfaces_exposed(bool);
    static bool expose_experimental_interfaces();
    static bool expose_experimental_interface(EnvironmentSettingsObject&, StringView name);

    [[nodiscard]] GC::Ref<Crypto::Crypto> crypto();
    [[nodiscard]] GC::Ref<ServiceWorker::CacheStorage> caches();
    GC::Ref<IndexedDB::IDBFactory> indexed_db();
    [[nodiscard]] GC::Ref<HighResolutionTime::Performance> performance();
    [[nodiscard]] GC::Ref<TrustedTypes::TrustedTypePolicyFactory> trusted_types();

protected:
    void initialize();
    void visit_edges(JS::Cell::Visitor&);
    void finalize();

    Optional<URL::Origin> window_or_worker_global_scope_extract_an_origin() const;

private:
    enum class Repeat {
        Yes,
        No,
    };
    i32 run_timer_initialization_steps(TimerHandler handler, i32 timeout, GC::RootVector<JS::Value> arguments, Repeat repeat, Optional<i32> previous_id = {});
    void run_steps_after_a_timeout_impl(i32 timeout, Function<void()> completion_step, Optional<i32> timer_key, Repeat repeat = Repeat::No);

    void create_image_bitmap_impl(JS::Realm&, GC::Ref<WebIDL::Promise>, ImageBitmapSource& image, Optional<WebIDL::Long> sx, Optional<WebIDL::Long> sy, Optional<WebIDL::Long> sw, Optional<WebIDL::Long> sh, ImageBitmapOptions options) const;

    size_t resource_timing_buffer_current_size();
    bool can_add_resource_timing_entry();
    void fire_resource_timing_buffer_full_event();
    void copy_resource_timing_secondary_buffer();

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
    OrderedHashMap<Utf16FlyString, PerformanceTimeline::PerformanceEntryTuple> m_performance_entry_buffer_map;

    HashTable<GC::Ref<EventSource>> m_registered_event_sources;

    GC::Ptr<HighResolutionTime::Performance> m_performance;

    GC::Ptr<IndexedDB::IDBFactory> m_indexed_db;

    GC::Ptr<Crypto::Crypto> m_crypto;

    GC::Ptr<ServiceWorker::CacheStorage> m_cache_storage;

    GC::Ptr<TrustedTypes::TrustedTypePolicyFactory> m_trusted_type_policy_factory;

    bool m_error_reporting_mode { false };

    WebSockets::WebSocket::List m_registered_web_sockets;

    // https://w3c.github.io/resource-timing/#sec-extensions-performance-interface
    // Each ECMAScript global environment has:
    // https://w3c.github.io/resource-timing/#dfn-resource-timing-buffer-size-limit
    // A resource timing buffer size limit which should initially be 250 or greater.
    // The recommended minimum number of PerformanceResourceTiming objects is 250, though this may be changed by the
    // user agent. setResourceTimingBufferSize can be called to request a change to this limit.
    u32 m_resource_timing_buffer_size_limit { 250 };

    // https://w3c.github.io/resource-timing/#dfn-resource-timing-buffer-full-event-pending-flag
    // A resource timing buffer full event pending flag which is initially false.
    bool m_resource_timing_buffer_full_event_pending { false };

    // https://w3c.github.io/resource-timing/#dfn-resource-timing-secondary-buffer-current-size
    // A resource timing secondary buffer current size which is initially 0.
    // https://w3c.github.io/resource-timing/#dfn-resource-timing-secondary-buffer
    // A resource timing secondary buffer to store PerformanceResourceTiming objects that is initially empty.
    Vector<GC::Ref<ResourceTiming::PerformanceResourceTiming>> m_resource_timing_secondary_buffer;

    // https://streams.spec.whatwg.org/#count-queuing-strategy-size-function
    GC::Ptr<WebIDL::CallbackType> m_count_queuing_strategy_size_function;

    // https://streams.spec.whatwg.org/#byte-length-queuing-strategy-size-function
    GC::Ptr<WebIDL::CallbackType> m_byte_length_queuing_strategy_size_function;

    // https://html.spec.whatwg.org/multipage/webappapis.html#about-to-be-notified-rejected-promises-list
    GC::Ptr<GC::HeapVector<GC::Ref<JS::Promise>>> m_about_to_be_notified_rejected_promises_list;

    // https://html.spec.whatwg.org/multipage/webappapis.html#outstanding-rejected-promises-weak-set
    // The outstanding rejected promises weak set must not create strong references to any of its members, and implementations are free to limit its size, e.g. by removing old entries from it when new ones are added.
    Vector<GC::Ptr<JS::Promise>> m_outstanding_rejected_promises_weak_set;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-global-import-map
    // A global object has an import map, initially an empty import map.
    ImportMap m_import_map;
};

}
