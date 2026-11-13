/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/IDAllocator.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Variant.h>
#include <LibCore/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/Scripting/ImportMap.h>
#include <LibWeb/HTML/Timer.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/WebSockets/WebSocket.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#timerhandler
using TimerHandler = Variant<Utf16String, GC::Ref<WebIDL::CallbackType>, GC::Ref<TrustedTypes::TrustedScript>>;

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

    WebIDL::ExceptionOr<i32> set_timeout(TimerHandler, i32 timeout, GC::RootVector<JS::Value> arguments);
    WebIDL::ExceptionOr<i32> set_interval(TimerHandler, i32 timeout, GC::RootVector<JS::Value> arguments);
    void clear_timeout(i32);
    void clear_interval(i32);
    void clear_map_of_active_timers();
    size_t active_timer_count(Badge<Internals::Internals>) const { return m_timers.size(); }
    Vector<i32> active_timer_ids(Badge<Internals::Internals>) const { return m_timers.keys(); }

    void queue_microtask(WebIDL::CallbackType&);

    void create_image_bitmap(JS::Realm&, ImageBitmapSource image, ImageBitmapOptions options, GC::Ref<WebIDL::Promise>) const;
    void create_image_bitmap(JS::Realm&, ImageBitmapSource image, WebIDL::Long sx, WebIDL::Long sy, WebIDL::Long sw, WebIDL::Long sh, ImageBitmapOptions options, GC::Ref<WebIDL::Promise>) const;

    WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Realm&, JS::Value, Bindings::StructuredSerializeOptions const&);

    enum class CheckIfPerformanceBufferIsFull {
        No,
        Yes,
    };

    void queue_performance_entry(GC::Ref<PerformanceTimeline::PerformanceEntry> new_entry);
    void add_performance_entry(GC::Ref<PerformanceTimeline::PerformanceEntry> new_entry, CheckIfPerformanceBufferIsFull check_if_performance_buffer_is_full = CheckIfPerformanceBufferIsFull::No);

    void register_event_source(Badge<EventSource>, GC::Ref<EventSource>);
    void register_permission_status(PermissionsAPI::PermissionStatus&);
    void unregister_event_source(Badge<EventSource>, GC::Ref<EventSource>);
    void forcibly_close_all_event_sources();

    void add_strong_reference_to_broadcast_channel(GC::Ref<DOM::EventTarget>);
    void remove_strong_reference_to_broadcast_channel(GC::Ref<DOM::EventTarget>);

    void close_all_idb_connections();

    void register_web_socket(Badge<WebSockets::WebSocket>, GC::Ref<WebSockets::WebSocket>);
    void unregister_web_socket(Badge<WebSockets::WebSocket>, GC::Ref<WebSockets::WebSocket>);

    enum class AffectedAnyWebSockets {
        No,
        Yes,
    };
    AffectedAnyWebSockets make_disappear_all_web_sockets();

    void set_has_pending_activity(DOM::EventTarget&, bool);
    bool has_pending_activity(DOM::EventTarget const&) const;
    void clear_objects_with_pending_activity() { m_objects_with_pending_activity.clear(); }

    i32 run_steps_after_a_timeout(i32 timeout, GC::Ref<GC::Function<void()>> completion_step);
    void cancel_timeout_steps(i32 timer_key);

    void document_visibility_state_changed(Badge<DOM::Document>);
    void document_audio_play_state_changed(Badge<DOM::Document>);
    void set_hidden_document_timer_wake_up_interval(Badge<Internals::Internals>, double milliseconds)
    {
        VERIFY(milliseconds > 0);
        m_hidden_document_timer_wake_up_interval = milliseconds;
    }
    void set_hidden_document_intensive_timer_throttling(Badge<Internals::Internals>, double wake_up_interval, double grace_period_once_loaded, double grace_period_while_loading)
    {
        VERIFY(wake_up_interval > 0 && grace_period_once_loaded >= 0 && grace_period_while_loading >= 0);
        m_intensive_timer_wake_up_interval = wake_up_interval;
        m_intensive_timer_throttling_grace_period_once_loaded = grace_period_once_loaded;
        m_intensive_timer_throttling_grace_period_while_loading = grace_period_while_loading;
    }

    void report_error(JS::Value e);

    bool is_in_error_reporting_mode() const { return m_in_error_reporting_mode; }
    void set_in_error_reporting_mode(bool value) { m_in_error_reporting_mode = value; }

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
    static bool expose_experimental_interface(EnvironmentSettingsObject const&, StringView name);

    [[nodiscard]] GC::Ref<Crypto::Crypto> crypto();
    [[nodiscard]] GC::Ref<ServiceWorker::CacheStorage> caches();
    GC::Ref<IndexedDB::IDBFactory> indexed_db();
    [[nodiscard]] GC::Ref<HighResolutionTime::Performance> performance();
    [[nodiscard]] GC::Ref<TrustedTypes::TrustedTypePolicyFactory> trusted_types();

protected:
    void visit_edges(JS::Cell::Visitor&);
    void finalize();

    Optional<URL::Origin> window_or_worker_global_scope_extract_an_origin() const;

private:
    enum class Repeat {
        Yes,
        No,
    };
    WebIDL::ExceptionOr<i32> run_timer_initialization_steps(TimerHandler handler, i32 timeout, GC::RootVector<JS::Value> arguments, Repeat repeat, Optional<i32> previous_id = {});
    bool has_expired_timer_yet_to_fire(double now);
    void run_steps_after_a_timeout_impl(i32 timeout, TimerThrottlingClass, GC::Ref<GC::Function<void()>> completion_step, Optional<i32> timer_key, Repeat repeat = Repeat::No);
    bool document_is_hidden() const;
    bool timers_are_throttled() const;
    Optional<i32> throttled_timer_delay(TimerThrottlingClass, double deadline) const;
    void realign_timers();

    void create_image_bitmap_impl(JS::Realm&, GC::Ref<WebIDL::Promise>, ImageBitmapSource& image, Optional<WebIDL::Long> sx, Optional<WebIDL::Long> sy, Optional<WebIDL::Long> sw, Optional<WebIDL::Long> sh, ImageBitmapOptions options) const;

    IDAllocator m_timer_id_allocator;
    HashMap<int, GC::Ref<Timer>> m_timers;
    HashTable<int> m_internal_timer_ids;
    double m_earliest_timer_deadline { NumericLimits<double>::max() };

    // The interval between the wake-ups a hidden document holds its delayed timers back to; a test can shorten it.
    double m_hidden_document_timer_wake_up_interval { 1000 };
    // The rarer wake-ups a hidden document holds its chained timers back to once it has been hidden for the grace
    // period — a minute for a document that had finished loading when it hid, five for one that was still loading,
    // which are Chrome's values; a test can shorten all three.
    double m_intensive_timer_wake_up_interval { 60'000 };
    double m_intensive_timer_throttling_grace_period_once_loaded { 60'000 };
    double m_intensive_timer_throttling_grace_period_while_loading { 300'000 };
    RefPtr<Core::Timer> m_intensive_timer_throttling_grace_timer;
    bool m_chained_timers_intensively_throttled { false };
    // When a chained timer last ran while the document was hidden (see throttled_timer_delay()).
    Optional<double> m_last_chained_timer_wake_up;

    HashTable<GC::Ref<EventSource>> m_registered_event_sources;

    // https://html.spec.whatwg.org/multipage/web-messaging.html#broadcasting-to-other-browsing-contexts
    // The open BroadcastChannel objects with a message or messageerror listener, which this global keeps alive. They're
    // held here rather than rooted — so a channel that's never closed can't keep its global alive.
    HashTable<GC::Ref<DOM::EventTarget>> m_strongly_referenced_broadcast_channels;
    Vector<GC::Weak<PermissionsAPI::PermissionStatus>> m_permission_statuses;

    GC::Ptr<IndexedDB::IDBFactory> m_indexed_db;

    GC::Ptr<Crypto::Crypto> m_crypto;

    GC::Ptr<ServiceWorker::CacheStorage> m_cache_storage;

    GC::Ptr<TrustedTypes::TrustedTypePolicyFactory> m_trusted_type_policy_factory;

    bool m_in_error_reporting_mode { false };

    WebSockets::WebSocket::List m_registered_web_sockets;

    // Objects with pending activity, such as a WebSocket with a listener for events still to come, which this global
    // keeps alive. They're held here rather than rooted, so they can't keep their global alive.
    HashTable<GC::Ref<DOM::EventTarget>> m_objects_with_pending_activity;

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
