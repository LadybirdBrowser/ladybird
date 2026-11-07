/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(AbortSignal);

extern "C" {
void* web_dom_abort_signal_from_wrapper(JSObject* wrapper);
JSObject* web_dom_abort_signal_wrapper(void* realm, DOM::AbortSignal*);
void* web_dom_create_abort_signal();
bool web_dom_abort_signal_aborted(DOM::AbortSignal*);
JSValue web_dom_abort_signal_reason(DOM::AbortSignal*);
bool web_dom_abort_signal_add_algorithm(DOM::AbortSignal*, JSObject* function, u64* id);
void web_dom_abort_signal_remove_algorithm(DOM::AbortSignal*, u64 id);
void web_dom_abort_signal_signal_abort(DOM::AbortSignal*, JSValue reason, void* realm);
}

GC::Ref<AbortSignal> AbortSignal::create()
{
    return GC::Heap::the().allocate<AbortSignal>();
}

AbortSignal::AbortSignal()
    : EventTarget()
{
}

// https://dom.spec.whatwg.org/#abortsignal-add
Optional<AbortSignal::AbortAlgorithmID> AbortSignal::add_abort_algorithm(GC::Ref<GC::Function<void()>> abort_algorithm)
{
    // 1. If signal is aborted, then return.
    if (aborted())
        return {};

    // 2. Append algorithm to signal’s abort algorithms.
    m_abort_algorithms.set(++m_next_abort_algorithm_id, abort_algorithm);
    return m_next_abort_algorithm_id;
}

// https://dom.spec.whatwg.org/#abortsignal-remove
void AbortSignal::remove_abort_algorithm(AbortAlgorithmID id)
{
    // To remove an algorithm algorithm from an AbortSignal signal, remove algorithm from signal’s abort algorithms.
    m_abort_algorithms.remove(id);
}

// https://dom.spec.whatwg.org/#abortsignal-signal-abort
void AbortSignal::signal_abort(JS::Value reason, JS::Object& relevant_global_object)
{
    // 1. If signal is aborted, then return.
    if (aborted())
        return;

    // 2. Set signal’s abort reason to reason if it is given; otherwise to a new "AbortError" DOMException.
    // Callers create the default DOMException at the JS boundary.
    VERIFY(!reason.is_undefined());
    m_abort_reason = reason;

    // 3. Let dependentSignalsToAbort be a new list.
    Vector<GC::Root<AbortSignal>> dependent_signals_to_abort;

    // 4. For each dependentSignal of signal’s dependent signals:
    for (auto const& dependent_signal : m_dependent_signals) {
        // 1. If dependentSignal is not aborted, then:
        if (!dependent_signal->aborted()) {
            // 1. Set dependentSignal’s abort reason to signal’s abort reason.
            dependent_signal->set_reason(m_abort_reason);

            // 2. Append dependentSignal to dependentSignalsToAbort.
            dependent_signals_to_abort.append(*dependent_signal);
        }
    }

    // https://dom.spec.whatwg.org/#run-the-abort-steps
    auto run_the_abort_steps = [&relevant_global_object](auto& signal) {
        // 1. For each algorithm in signal’s abort algorithms: run algorithm.
        for (auto const& algorithm : signal.m_abort_algorithms)
            algorithm.value->function()();

        // 2. Empty signal’s abort algorithms.
        signal.m_abort_algorithms.clear();

        // 3. Fire an event named abort at signal.
        auto abort_event = Event::create(HTML::EventNames::abort, HighResolutionTime::current_high_resolution_time(relevant_global_object));
        abort_event->set_is_trusted(true);
        signal.dispatch_event(abort_event);
    };

    // 5. Run the abort steps for signal.
    run_the_abort_steps(*this);

    // 6. For each dependentSignal of dependentSignalsToAbort, run the abort steps for dependentSignal.
    for (auto const& dependent_signal : dependent_signals_to_abort)
        run_the_abort_steps(*dependent_signal);
}

void AbortSignal::set_onabort(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::abort, event_handler);
}

WebIDL::CallbackType* AbortSignal::onabort()
{
    return event_handler_attribute(HTML::EventNames::abort);
}

// https://dom.spec.whatwg.org/#dom-abortsignal-throwifaborted
JS::ThrowCompletionOr<void> AbortSignal::throw_if_aborted() const
{
    // The throwIfAborted() method steps are to throw this’s abort reason, if this is aborted.
    if (!aborted())
        return {};

    return JS::throw_completion(m_abort_reason);
}

void AbortSignal::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_abort_reason);
    visitor.visit(m_abort_algorithms);
    visitor.visit(m_source_signals);
    visitor.visit(m_dependent_signals);
}

size_t AbortSignal::external_memory_size() const
{
    auto size = Base::external_memory_size();
    size = JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_abort_algorithms));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_source_signals));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_dependent_signals));
    return size;
}

// https://dom.spec.whatwg.org/#dom-abortsignal-abort
WebIDL::ExceptionOr<GC::Ref<AbortSignal>> AbortSignal::abort(JS::Realm& realm, Optional<JS::Value> maybe_reason)
{
    // 1. Let signal be a new AbortSignal object.
    auto signal = create();

    // 2. Set signal’s abort reason to reason if it is given; otherwise to a new "AbortError" DOMException.
    auto reason = maybe_reason.value_or(JS::js_undefined());
    if (reason.is_undefined())
        reason = throw_completion(realm, WebIDL::AbortError::create("Aborted without reason"_utf16)).value();

    signal->set_reason(reason);

    // 3. Return signal.
    return signal;
}

// https://dom.spec.whatwg.org/#dom-abortsignal-timeout
WebIDL::ExceptionOr<GC::Ref<AbortSignal>> AbortSignal::timeout(JS::Realm& realm, WebIDL::UnsignedLongLong milliseconds)
{
    // 1. Let signal be a new AbortSignal object.
    auto signal = create();

    // 2. Let global be signal’s relevant global object.
    auto& global = realm.global_object();
    auto* window_or_worker = HTML::window_or_worker_global_scope_from_global_object(global);
    VERIFY(window_or_worker);

    // 3. Run steps after a timeout given global, "AbortSignal-timeout", milliseconds, and the following step:
    window_or_worker->run_steps_after_a_timeout(milliseconds, GC::create_function(GC::Heap::the(), GC::captures(realm, global, signal), [](auto& realm, auto& global, auto& signal) {
        // 1. Queue a global task on the timer task source given global to signal abort given signal and a new "TimeoutError" DOMException.
        HTML::queue_global_task(HTML::Task::Source::TimerTask, global, GC::create_function(GC::Heap::the(), GC::captures(realm, global, signal), [](auto& realm, auto& global, auto& signal) {
            auto reason = throw_completion(realm, WebIDL::TimeoutError::create("Signal timed out"_utf16)).value();
            signal->signal_abort(reason, global);
        }));
    }));

    // 4. Return signal.
    return signal;
}

// https://dom.spec.whatwg.org/#dom-abortsignal-any
WebIDL::ExceptionOr<GC::Ref<AbortSignal>> AbortSignal::any(ReadonlySpan<GC::Ref<AbortSignal>> signals)
{
    // The static any(signals) method steps are to return the result of creating a dependent abort signal from signals using AbortSignal and the current realm.
    return create_dependent_abort_signal(signals);
}

// https://dom.spec.whatwg.org/#create-a-dependent-abort-signal
WebIDL::ExceptionOr<GC::Ref<AbortSignal>> AbortSignal::create_dependent_abort_signal(ReadonlySpan<GC::Ref<AbortSignal>> signals)
{
    // 1. Let resultSignal be a new object implementing signalInterface using realm.
    auto result_signal = create();

    // 2. For each signal of signals: if signal is aborted, then set resultSignal’s abort reason to signal’s abort reason and return resultSignal.
    for (auto const& signal : signals) {
        if (signal->aborted()) {
            result_signal->set_reason(signal->reason());
            return result_signal;
        }
    }

    // 3. Set resultSignal’s dependent to true.
    result_signal->set_dependent(true);

    // 4. For each signal of signals:
    for (auto const& signal : signals) {
        // 1. If signal’s dependent is false, then:
        if (!signal->dependent()) {
            // 1. Append signal to resultSignal’s source signals.
            result_signal->append_source_signal({ signal });

            // 2. Append resultSignal to signal’s dependent signals.
            signal->append_dependent_signal(result_signal);
        }
        // 2. Otherwise, for each sourceSignal of signal’s source signals:
        else {
            for (auto const& source_signal : signal->source_signals()) {
                // 1. Assert: sourceSignal is not aborted and not dependent.
                VERIFY(source_signal);
                VERIFY(!source_signal->aborted());
                VERIFY(!source_signal->dependent());

                // 2. Append sourceSignal to resultSignal’s source signals.
                result_signal->append_source_signal(source_signal);

                // 3. Append resultSignal to sourceSignal’s dependent signals.
                source_signal->append_dependent_signal(result_signal);
            }
        }
    }

    // 5. Return resultSignal
    return result_signal;
}

void* web_dom_abort_signal_from_wrapper(JSObject* wrapper)
{
    auto* signal = Bindings::impl_from<DOM::AbortSignal>(JS::HostABI::object_from_abi(wrapper));
    VERIFY(signal);
    return signal;
}

JSObject* web_dom_abort_signal_wrapper(void* realm, DOM::AbortSignal* signal)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    return JS::HostABI::object_to_abi(Bindings::wrap(Bindings::host_defined_wrapper_world(js_realm), js_realm, GC::Ref { *signal }).ptr());
}

void* web_dom_create_abort_signal()
{
    return DOM::AbortSignal::create().ptr();
}

bool web_dom_abort_signal_aborted(DOM::AbortSignal* signal)
{
    return signal->aborted();
}

JSValue web_dom_abort_signal_reason(DOM::AbortSignal* signal)
{
    return JS::HostABI::value_to_abi(signal->reason());
}

bool web_dom_abort_signal_add_algorithm(DOM::AbortSignal* signal, JSObject* function, u64* id)
{
    GC::Ref<JS::FunctionObject> algorithm = as<JS::FunctionObject>(*JS::HostABI::object_from_abi(function));
    auto algorithm_id = signal->add_abort_algorithm(GC::create_function(GC::Heap::the(), GC::captures(algorithm), [](GC::Ref<JS::FunctionObject> algorithm) {
        // NB: The algorithm cannot fail, but it can run into the abort of the script that it calls.
        auto& vm = algorithm->vm();
        auto result = JS::call(vm, *algorithm, JS::js_undefined());
        VERIFY(!result.is_error() || vm.interrupt_requested());
    }));
    if (!algorithm_id.has_value())
        return false;
    *id = algorithm_id.value();
    return true;
}

void web_dom_abort_signal_remove_algorithm(DOM::AbortSignal* signal, u64 id)
{
    signal->remove_abort_algorithm(id);
}

void web_dom_abort_signal_signal_abort(DOM::AbortSignal* signal, JSValue reason, void* realm)
{
    signal->signal_abort(JS::HostABI::value_from_abi(reason), static_cast<JS::Realm*>(realm)->global_object());
}

}
