/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AbortSignalPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(AbortSignal);

WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> AbortSignal::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<AbortSignal>(realm, realm);
}

AbortSignal::AbortSignal(JS::Realm& realm)
    : EventTarget(realm)
{
}

void AbortSignal::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AbortSignal);
}

// https://dom.spec.whatwg.org/#abortsignal-add
void AbortSignal::add_abort_algorithm(Function<void()> abort_algorithm)
{
    // 1. If signal is aborted, then return.
    if (aborted())
        return;

    // 2. Append algorithm to signal’s abort algorithms.
    m_abort_algorithms.append(JS::create_heap_function(vm().heap(), move(abort_algorithm)));
}

// https://dom.spec.whatwg.org/#abortsignal-signal-abort
void AbortSignal::signal_abort(JS::Value reason)
{
    // 1. If signal is aborted, then return.
    if (aborted())
        return;

    // 2. Set signal’s abort reason to reason if it is given; otherwise to a new "AbortError" DOMException.
    if (!reason.is_undefined())
        m_abort_reason = reason;
    else
        m_abort_reason = WebIDL::AbortError::create(realm(), "Aborted without reason"_string).ptr();

    // 3. Let dependentSignalsToAbort be a new list.
    Vector<JS::Handle<AbortSignal>> dependent_signals_to_abort;

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
    auto run_the_abort_steps = [](auto& signal) {
        // 1. For each algorithm in signal’s abort algorithms: run algorithm.
        for (auto& algorithm : signal.m_abort_algorithms)
            algorithm->function()();

        // 2. Empty signal’s abort algorithms.
        signal.m_abort_algorithms.clear();

        // 3. Fire an event named abort at signal.
        auto abort_event = Event::create(signal.realm(), HTML::EventNames::abort);
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

// https://dom.spec.whatwg.org/#dom-abortsignal-abort
WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> AbortSignal::abort(JS::VM& vm, JS::Value reason)
{
    // 1. Let signal be a new AbortSignal object.
    auto signal = TRY(construct_impl(*vm.current_realm()));

    // 2. Set signal’s abort reason to reason if it is given; otherwise to a new "AbortError" DOMException.
    if (reason.is_undefined())
        reason = WebIDL::AbortError::create(*vm.current_realm(), "Aborted without reason"_string).ptr();

    signal->set_reason(reason);

    // 3. Return signal.
    return signal;
}

// https://dom.spec.whatwg.org/#dom-abortsignal-timeout
WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> AbortSignal::timeout(JS::VM& vm, WebIDL::UnsignedLongLong milliseconds)
{
    auto& realm = *vm.current_realm();

    // 1. Let signal be a new AbortSignal object.
    auto signal = TRY(construct_impl(realm));

    // 2. Let global be signal’s relevant global object.
    auto& global = HTML::relevant_global_object(signal);
    auto* window_or_worker = dynamic_cast<HTML::WindowOrWorkerGlobalScopeMixin*>(&global);
    VERIFY(window_or_worker);

    // 3. Run steps after a timeout given global, "AbortSignal-timeout", milliseconds, and the following step:
    window_or_worker->run_steps_after_a_timeout(milliseconds, [&realm, &global, signal]() {
        // 1. Queue a global task on the timer task source given global to signal abort given signal and a new "TimeoutError" DOMException.
        HTML::queue_global_task(HTML::Task::Source::TimerTask, global, JS::create_heap_function(realm.heap(), [&realm, signal]() mutable {
            auto reason = WebIDL::TimeoutError::create(realm, "Signal timed out"_string);
            signal->signal_abort(reason);
        }));
    });

    // 4. Return signal.
    return signal;
}

// https://dom.spec.whatwg.org/#dom-abortsignal-any
WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> AbortSignal::any(JS::VM& vm, Vector<JS::Handle<AbortSignal>> const& signals)
{
    // The static any(signals) method steps are to return the result of creating a dependent abort signal from signals using AbortSignal and the current realm.
    return create_dependent_abort_signal(*vm.current_realm(), signals);
}

// https://dom.spec.whatwg.org/#create-a-dependent-abort-signal
WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> AbortSignal::create_dependent_abort_signal(JS::Realm& realm, Vector<JS::Handle<AbortSignal>> const& signals)
{
    // 1. Let resultSignal be a new object implementing signalInterface using realm.
    auto result_signal = TRY(construct_impl(realm));

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

}
