/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/MutationObserverPrototype.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/DOM/Node.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(MutationObserver);
JS_DEFINE_ALLOCATOR(TransientRegisteredObserver);

WebIDL::ExceptionOr<JS::NonnullGCPtr<MutationObserver>> MutationObserver::construct_impl(JS::Realm& realm, JS::GCPtr<WebIDL::CallbackType> callback)
{
    return realm.heap().allocate<MutationObserver>(realm, realm, callback);
}

// https://dom.spec.whatwg.org/#dom-mutationobserver-mutationobserver
MutationObserver::MutationObserver(JS::Realm& realm, JS::GCPtr<WebIDL::CallbackType> callback)
    : PlatformObject(realm)
    , m_callback(move(callback))
{

    // 1. Set this’s callback to callback.

    // 2. Append this to this’s relevant agent’s mutation observers.
    auto* agent_custom_data = verify_cast<Bindings::WebEngineCustomData>(realm.vm().custom_data());
    agent_custom_data->mutation_observers.append(*this);
}

MutationObserver::~MutationObserver()
{
    auto* agent_custom_data = verify_cast<Bindings::WebEngineCustomData>(vm().custom_data());
    agent_custom_data->mutation_observers.remove_all_matching([this](auto& observer) {
        return observer.ptr() == this;
    });
}

void MutationObserver::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MutationObserver);
}

void MutationObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_record_queue);
}

// https://dom.spec.whatwg.org/#dom-mutationobserver-observe
WebIDL::ExceptionOr<void> MutationObserver::observe(Node& target, MutationObserverInit options)
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
                if (node.is_null())
                    continue;

                if (node->registered_observer_list()) {
                    node->registered_observer_list()->remove_all_matching([&registered_observer](RegisteredObserver& observer) {
                        return is<TransientRegisteredObserver>(observer) && verify_cast<TransientRegisteredObserver>(observer).source().ptr() == registered_observer;
                    });
                }
            }

            // 2. Set registered’s options to options.
            registered_observer->set_options(options);
            break;
        }
    }

    // 8. Otherwise:
    if (!updated_existing_observer) {
        // 1. Append a new registered observer whose observer is this and options is options to target’s registered observer list.
        auto new_registered_observer = RegisteredObserver::create(*this, options);
        target.add_registered_observer(new_registered_observer);

        // 2. Append target to this’s node list.
        m_node_list.append(target.make_weak_ptr<Node>());
    }

    return {};
}

// https://dom.spec.whatwg.org/#dom-mutationobserver-disconnect
void MutationObserver::disconnect()
{
    // 1. For each node of this’s node list, remove any registered observer from node’s registered observer list for which this is the observer.
    for (auto& node : m_node_list) {
        // FIXME: Is this correct?
        if (node.is_null())
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
Vector<JS::Handle<MutationRecord>> MutationObserver::take_records()
{
    // 1. Let records be a clone of this’s record queue.
    Vector<JS::Handle<MutationRecord>> records;
    for (auto& record : m_record_queue)
        records.append(*record);

    // 2. Empty this’s record queue.
    m_record_queue.clear();

    // 3. Return records.
    return records;
}

JS::NonnullGCPtr<RegisteredObserver> RegisteredObserver::create(MutationObserver& observer, MutationObserverInit const& options)
{
    return observer.heap().allocate_without_realm<RegisteredObserver>(observer, options);
}

RegisteredObserver::RegisteredObserver(MutationObserver& observer, MutationObserverInit const& options)
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

JS::NonnullGCPtr<TransientRegisteredObserver> TransientRegisteredObserver::create(MutationObserver& observer, MutationObserverInit const& options, RegisteredObserver& source)
{
    return observer.heap().allocate_without_realm<TransientRegisteredObserver>(observer, options, source);
}

TransientRegisteredObserver::TransientRegisteredObserver(MutationObserver& observer, MutationObserverInit const& options, RegisteredObserver& source)
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
