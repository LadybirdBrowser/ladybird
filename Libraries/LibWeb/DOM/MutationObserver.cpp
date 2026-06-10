/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/MutationRecord.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(MutationObserver);
GC_DEFINE_ALLOCATOR(RegisteredObserver);
GC_DEFINE_ALLOCATOR(TransientRegisteredObserver);

WebIDL::ExceptionOr<GC::Ref<MutationObserver>> MutationObserver::create(GC::Ptr<WebIDL::CallbackType> callback)
{
    return GC::Heap::the().allocate<MutationObserver>(callback);
}

// https://dom.spec.whatwg.org/#dom-mutationobserver-mutationobserver
MutationObserver::MutationObserver(GC::Ptr<WebIDL::CallbackType> callback)
    : m_callback(move(callback))
{
    // The new MutationObserver(callback) constructor steps are to set this’s callback to callback.
}

MutationObserver::~MutationObserver() = default;

void MutationObserver::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_record_queue);
}

static MutationObserverOptions to_dom_options(Bindings::MutationObserverInit const& options)
{
    return {
        .attribute_filter = options.attribute_filter,
        .attribute_old_value = options.attribute_old_value,
        .attributes = options.attributes,
        .character_data = options.character_data,
        .character_data_old_value = options.character_data_old_value,
        .child_list = options.child_list,
        .subtree = options.subtree,
    };
}

static void invoke_mutation_observer_callback(MutationObserver& mutation_observer, Vector<GC::Root<MutationRecord>>& records)
{
    auto& callback = mutation_observer.callback();
    auto& settings = callback.callback_context;

    auto wrapped_records = MUST(JS::Array::create(settings->realm(), 0));
    auto& wrapper_world = Bindings::host_defined_wrapper_world(settings->realm());
    for (size_t i = 0; i < records.size(); ++i) {
        auto& record = records.at(i);
        auto property_index = JS::PropertyKey { i };
        MUST(wrapped_records->create_data_property(property_index, Bindings::wrap(wrapper_world, settings->realm(), GC::Ref { *record })));
    }

    auto wrapped_mutation_observer = Bindings::wrap(wrapper_world, settings->realm(), GC::Ref { mutation_observer });
    (void)WebIDL::invoke_callback(callback, wrapped_mutation_observer, WebIDL::ExceptionBehavior::Report, { { wrapped_records, wrapped_mutation_observer } });
}

// https://dom.spec.whatwg.org/#dom-mutationobserver-observe
WebIDL::ExceptionOr<void> MutationObserver::observe(Node& target, MutationObserverOptions options)
{
    // 1. If either options["attributeOldValue"] or options["attributeFilter"] exists, and options["attributes"] does not exist, then set options["attributes"] to true.
    if ((options.attribute_old_value.has_value() || options.attribute_filter.has_value()) && !options.attributes.has_value())
        options.attributes = true;

    // 2. If options["characterDataOldValue"] exists and options["characterData"] does not exist, then set options["characterData"] to true.
    if (options.character_data_old_value.has_value() && !options.character_data.has_value())
        options.character_data = true;

    // 3. If none of options["childList"], options["attributes"], and options["characterData"] is true, then throw a TypeError.
    if (!options.child_list && (!options.attributes.has_value() || !options.attributes.value()) && (!options.character_data.has_value() || !options.character_data.value()))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Options must have one of childList, attributes or characterData set to true."sv };

    // 4. If options["attributeOldValue"] is true and options["attributes"] is false, then throw a TypeError.
    // NOTE: If attributeOldValue is present, attributes will be present because of step 1.
    if (options.attribute_old_value.has_value() && options.attribute_old_value.value() && !options.attributes.value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "attributes must be true if attributeOldValue is true."sv };

    // 5. If options["attributeFilter"] is present and options["attributes"] is false, then throw a TypeError.
    // NOTE: If attributeFilter is present, attributes will be present because of step 1.
    if (options.attribute_filter.has_value() && !options.attributes.value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "attributes must be true if attributeFilter is present."sv };

    // 6. If options["characterDataOldValue"] is true and options["characterData"] is false, then throw a TypeError.
    // NOTE: If characterDataOldValue is present, characterData will be present because of step 2.
    if (options.character_data_old_value.has_value() && options.character_data_old_value.value() && !options.character_data.value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "characterData must be true if characterDataOldValue is true."sv };

    // 7. For each registered of target’s registered observer list, if registered’s observer is this:
    bool updated_existing_observer = false;
    if (target.registered_observer_list()) {
        for (auto& registered_observer : *target.registered_observer_list()) {
            if (registered_observer->observer().ptr() != this)
                continue;

            updated_existing_observer = true;

            // 1. For each node of this’s node list, remove all transient registered observers whose source is registered from node’s registered observer list.
            for (auto& node : m_node_list) {
                // FIXME: Is this correct?
                if (!node)
                    continue;

                if (node->registered_observer_list()) {
                    node->registered_observer_list()->remove_all_matching([&registered_observer](RegisteredObserver& observer) {
                        auto* transient = as_if<TransientRegisteredObserver>(observer);
                        return transient && transient->source().ptr() == registered_observer;
                    });
                }
            }

            // 2. Set registered’s options to options.
            registered_observer->set_options(move(options));
            break;
        }
    }

    // 8. Otherwise:
    if (!updated_existing_observer) {
        // 1. Append a new registered observer whose observer is this and options is options to target’s registered observer list.
        auto new_registered_observer = RegisteredObserver::create(*this, options);
        target.add_registered_observer(new_registered_observer);

        // 2. Append a weak reference to target to this’s node list.
        m_node_list.append(target);
    }

    return {};
}

WebIDL::ExceptionOr<void> MutationObserver::observe(Node& target, Bindings::MutationObserverInit const& options)
{
    return observe(target, to_dom_options(options));
}

// https://dom.spec.whatwg.org/#dom-mutationobserver-disconnect
void MutationObserver::disconnect()
{
    // 1. For each node of this’s node list, remove any registered observer from node’s registered observer list for which this is the observer.
    for (auto& node : m_node_list) {
        // FIXME: Is this correct?
        if (!node)
            continue;

        if (node->registered_observer_list()) {
            node->registered_observer_list()->remove_all_matching([this](RegisteredObserver& registered_observer) {
                return registered_observer.observer().ptr() == this;
            });
        }
    }

    // 2. Empty this’s record queue.
    m_record_queue.clear();
}

// https://dom.spec.whatwg.org/#dom-mutationobserver-takerecords
Vector<GC::Root<MutationRecord>> MutationObserver::take_records()
{
    // 1. Let records be a clone of this’s record queue.
    Vector<GC::Root<MutationRecord>> records;
    for (auto& record : m_record_queue)
        records.append(*record);

    // 2. Empty this’s record queue.
    m_record_queue.clear();

    // 3. Return records.
    return records;
}

// https://dom.spec.whatwg.org/#queue-a-mutation-observer-compound-microtask
void queue_mutation_observer_microtask(HTML::SimilarOriginWindowAgent& surrounding_agent)
{
    // 1. If the surrounding agent’s mutation observer microtask queued is true, then return.
    if (surrounding_agent.mutation_observer_microtask_queued)
        return;

    // 2. Set the surrounding agent’s mutation observer microtask queued to true.
    surrounding_agent.mutation_observer_microtask_queued = true;

    // 3. Queue a microtask to notify mutation observers.
    HTML::queue_a_microtask(nullptr, GC::create_function(GC::Heap::the(), [&surrounding_agent]() {
        // https://dom.spec.whatwg.org/#notify-mutation-observers
        // 1. Set the surrounding agent’s mutation observer microtask queued to false.
        surrounding_agent.mutation_observer_microtask_queued = false;

        // 2. Let notifySet be a clone of the surrounding agent’s pending mutation observers.
        // 3. Empty the surrounding agent’s pending mutation observers.
        auto notify_set = move(surrounding_agent.pending_mutation_observers);

        // 4. Let signalSet be a clone of the surrounding agent’s signal slots.
        // 5. Empty the surrounding agent’s signal slots.
        auto signal_set = move(surrounding_agent.signal_slots);

        // 6. For each mo of notifySet:
        for (auto& mutation_observer : notify_set) {
            // 1. Let records be a clone of mo’s record queue.
            // 2. Empty mo’s record queue.
            auto records = mutation_observer->take_records();

            // 3. For each node of mo’s node list, remove all transient registered observers whose observer is mo from
            //    node’s registered observer list.
            for (auto& node : mutation_observer->node_list()) {
                // FIXME: Is this correct?
                if (!node)
                    continue;

                if (node->registered_observer_list()) {
                    node->registered_observer_list()->remove_all_matching([&mutation_observer](RegisteredObserver& registered_observer) {
                        return is<TransientRegisteredObserver>(registered_observer) && static_cast<TransientRegisteredObserver&>(registered_observer).observer().ptr() == mutation_observer;
                    });
                }
            }

            // 4. If records is not empty, then invoke mo’s callback with « records, mo » and "report", and with
            //    callback this value mo.
            if (!records.is_empty()) {
                invoke_mutation_observer_callback(*mutation_observer, records);
            }
        }

        // 7. For each slot of signalSet, fire an event named slotchange, with its bubbles attribute set to true, at slot.
        for (auto& slot : signal_set) {
            EventInit event_init;
            event_init.bubbles = true;
            slot->dispatch_event(Event::create(HTML::EventNames::slotchange, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*slot))));
        }
    }));
}

GC::Ref<RegisteredObserver> RegisteredObserver::create(MutationObserver& observer, MutationObserverOptions const& options)
{
    return GC::Heap::the().allocate<RegisteredObserver>(observer, options);
}

RegisteredObserver::RegisteredObserver(MutationObserver& observer, MutationObserverOptions const& options)
    : m_observer(observer)
    , m_options(options)
{
}

RegisteredObserver::~RegisteredObserver() = default;

void RegisteredObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_observer);
}

GC::Ref<TransientRegisteredObserver> TransientRegisteredObserver::create(MutationObserver& observer, MutationObserverOptions const& options, RegisteredObserver& source)
{
    return GC::Heap::the().allocate<TransientRegisteredObserver>(observer, options, source);
}

TransientRegisteredObserver::TransientRegisteredObserver(MutationObserver& observer, MutationObserverOptions const& options, RegisteredObserver& source)
    : RegisteredObserver(observer, options)
    , m_source(source)
{
}

TransientRegisteredObserver::~TransientRegisteredObserver() = default;

void TransientRegisteredObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source);
}

}
