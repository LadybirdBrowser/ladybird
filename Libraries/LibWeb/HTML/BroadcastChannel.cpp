/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/BroadcastChannelPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/BroadcastChannel.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::HTML {

class BroadcastChannelRepository {
public:
    void register_channel(GC::Root<BroadcastChannel>);
    void unregister_channel(GC::Ref<BroadcastChannel>);
    Vector<GC::Root<BroadcastChannel>> const& registered_channels_for_key(StorageAPI::StorageKey const&) const;

private:
    HashMap<StorageAPI::StorageKey, Vector<GC::Root<BroadcastChannel>>> m_channels;
};

void BroadcastChannelRepository::register_channel(GC::Root<BroadcastChannel> channel)
{
    auto storage_key = Web::StorageAPI::obtain_a_storage_key_for_non_storage_purposes(relevant_settings_object(*channel));

    auto maybe_channels = m_channels.find(storage_key);
    if (maybe_channels != m_channels.end()) {
        maybe_channels->value.append(move(channel));
        return;
    }

    Vector<GC::Root<BroadcastChannel>> channels;
    channels.append(move(channel));
    m_channels.set(storage_key, move(channels));
}

void BroadcastChannelRepository::unregister_channel(GC::Ref<BroadcastChannel> channel)
{
    auto storage_key = Web::StorageAPI::obtain_a_storage_key_for_non_storage_purposes(relevant_settings_object(channel));
    auto& relevant_channels = m_channels.get(storage_key).value();
    relevant_channels.remove_first_matching([&](auto const& c) { return c == channel; });
}

Vector<GC::Root<BroadcastChannel>> const& BroadcastChannelRepository::registered_channels_for_key(StorageAPI::StorageKey const& key) const
{
    auto maybe_channels = m_channels.get(key);
    VERIFY(maybe_channels.has_value());
    return maybe_channels.value();
}

// FIXME: This should not be static, and live at a storage partitioned level of the user agent.
static BroadcastChannelRepository s_broadcast_channel_repository;

GC_DEFINE_ALLOCATOR(BroadcastChannel);

GC::Ref<BroadcastChannel> BroadcastChannel::construct_impl(JS::Realm& realm, FlyString const& name)
{
    auto channel = realm.create<BroadcastChannel>(realm, name);
    s_broadcast_channel_repository.register_channel(channel);
    return channel;
}

BroadcastChannel::BroadcastChannel(JS::Realm& realm, FlyString const& name)
    : DOM::EventTarget(realm)
    , m_channel_name(name)
{
}

void BroadcastChannel::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BroadcastChannel);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#eligible-for-messaging
bool BroadcastChannel::is_eligible_for_messaging() const
{
    // A BroadcastChannel object is said to be eligible for messaging when its relevant global object is either:
    auto const& global = relevant_global_object(*this);

    // * a Window object whose associated Document is fully active, or
    if (is<Window>(global))
        return static_cast<Window const&>(global).associated_document().is_fully_active();

    // * a WorkerGlobalScope object whose closing flag is false and whose worker is not a suspendable worker.
    // FIXME: Suspendable worker
    if (is<WorkerGlobalScope>(global))
        return !static_cast<WorkerGlobalScope const&>(global).is_closing();

    return false;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-postmessage
WebIDL::ExceptionOr<void> BroadcastChannel::post_message(JS::Value message)
{
    auto& vm = this->vm();

    // 1. If this is not eligible for messaging, then return.
    if (!is_eligible_for_messaging())
        return {};

    // 2. If this's closed flag is true, then throw an "InvalidStateError" DOMException.
    if (m_closed_flag)
        return WebIDL::InvalidStateError::create(realm(), "BroadcastChannel.postMessage() on a closed channel"_utf16);

    // 3. Let serialized be StructuredSerialize(message). Rethrow any exceptions.
    auto serialized = TRY(structured_serialize(vm, message));

    // 4. Let sourceOrigin be this's relevant settings object's origin.
    auto source_origin = relevant_settings_object(*this).origin();

    // 5. Let sourceStorageKey be the result of running obtain a storage key for non-storage purposes with this's relevant settings object.
    auto source_storage_key = Web::StorageAPI::obtain_a_storage_key_for_non_storage_purposes(relevant_settings_object(*this));

    // 6. Let destinations be a list of BroadcastChannel objects that match the following criteria:
    GC::RootVector<GC::Ref<BroadcastChannel>> destinations(vm.heap());

    // * The result of running obtain a storage key for non-storage purposes with their relevant settings object equals sourceStorageKey.
    auto same_origin_broadcast_channels = s_broadcast_channel_repository.registered_channels_for_key(source_storage_key);

    for (auto const& channel : same_origin_broadcast_channels) {
        // * They are eligible for messaging.
        if (!channel->is_eligible_for_messaging())
            continue;

        // * Their channel name is this's channel name.
        if (channel->name() != name())
            continue;

        destinations.append(*channel);
    }

    // 7. Remove source from destinations.
    destinations.remove_first_matching([&](auto destination) { return destination == this; });

    // FIXME: 8. Sort destinations such that all BroadcastChannel objects whose relevant agents are the same are sorted in creation order, oldest first.
    //    (This does not define a complete ordering. Within this constraint, user agents may sort the list in any implementation-defined manner.)

    // 9. For each destination in destinations, queue a global task on the DOM manipulation task source given destination's relevant global object to perform the following steps:
    for (auto destination : destinations) {
        HTML::queue_global_task(HTML::Task::Source::DOMManipulation, relevant_global_object(destination), GC::create_function(vm.heap(), [&vm, serialized, destination, source_origin] {
            // 1. If destination's closed flag is true, then abort these steps.
            if (destination->m_closed_flag)
                return;

            // 2. Let targetRealm be destination's relevant realm.
            auto& target_realm = relevant_realm(destination);

            // 3. Let data be StructuredDeserialize(serialized, targetRealm).
            //    If this throws an exception, catch it, fire an event named messageerror at destination, using MessageEvent, with the
            //    origin attribute initialized to the serialization of sourceOrigin, and then abort these steps.
            auto data_or_error = structured_deserialize(vm, serialized, target_realm);
            if (data_or_error.is_exception()) {
                MessageEventInit event_init {};
                event_init.origin = source_origin.serialize();
                auto event = MessageEvent::create(target_realm, HTML::EventNames::messageerror, event_init);
                event->set_is_trusted(true);
                destination->dispatch_event(event);
                return;
            }

            // 4. Fire an event named message at destination, using MessageEvent, with the data attribute initialized to data and the
            //    origin attribute initialized to the serialization of sourceOrigin.
            MessageEventInit event_init {};
            event_init.data = data_or_error.release_value();
            event_init.origin = source_origin.serialize();
            auto event = MessageEvent::create(target_realm, HTML::EventNames::message, event_init);
            event->set_is_trusted(true);
            destination->dispatch_event(event);
        }));
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-close
void BroadcastChannel::close()
{
    // The close() method steps are to set this's closed flag to true.
    m_closed_flag = true;

    s_broadcast_channel_repository.unregister_channel(*this);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessage
void BroadcastChannel::set_onmessage(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::message, event_handler);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessage
GC::Ptr<WebIDL::CallbackType> BroadcastChannel::onmessage()
{
    return event_handler_attribute(HTML::EventNames::message);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessageerror
void BroadcastChannel::set_onmessageerror(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::messageerror, event_handler);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessageerror
GC::Ptr<WebIDL::CallbackType> BroadcastChannel::onmessageerror()
{
    return event_handler_attribute(HTML::EventNames::messageerror);
}

}
