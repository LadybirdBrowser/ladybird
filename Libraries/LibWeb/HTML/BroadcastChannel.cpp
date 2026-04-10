/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibCore/System.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/BroadcastChannelPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BroadcastChannel.h>
#include <LibWeb/HTML/BroadcastChannelMessage.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/Worker/WebWorkerClient.h>

namespace Web::HTML {

class BroadcastChannelRepository {
public:
    void register_channel(GC::Ref<BroadcastChannel>);
    void unregister_channel(GC::Ref<BroadcastChannel>);
    auto const& registered_channels_for_key(StorageAPI::StorageKey) const;
    [[nodiscard]] u64 next_channel_id() { return ++m_next_channel_id; }

private:
    HashMap<StorageAPI::StorageKey, Vector<GC::Weak<BroadcastChannel>>> m_channels;
    u64 m_next_channel_id { 0 };
};

void BroadcastChannelRepository::register_channel(GC::Ref<BroadcastChannel> channel)
{
    auto storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(relevant_settings_object(*channel));
    channel->m_channel_id = next_channel_id();
    m_channels.ensure(storage_key).append(channel);
}

void BroadcastChannelRepository::unregister_channel(GC::Ref<BroadcastChannel> channel)
{
    auto storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(relevant_settings_object(channel));
    auto maybe_channels = m_channels.get(storage_key);
    if (!maybe_channels.has_value())
        return;

    auto& relevant_channels = maybe_channels.value();
    relevant_channels.remove_first_matching([&](auto c) { return c == channel; });
    if (relevant_channels.is_empty())
        m_channels.remove(storage_key);
}

auto const& BroadcastChannelRepository::registered_channels_for_key(StorageAPI::StorageKey key) const
{
    static Vector<GC::Weak<BroadcastChannel>> s_empty_channels;

    auto maybe_channels = m_channels.get(key);
    if (!maybe_channels.has_value())
        return s_empty_channels;

    return maybe_channels.value();
}

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

void BroadcastChannel::finalize()
{
    Base::finalize();
    s_broadcast_channel_repository.unregister_channel(*this);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#eligible-for-messaging
bool BroadcastChannel::is_eligible_for_messaging() const
{
    // A BroadcastChannel object is said to be eligible for messaging when its relevant global object is either:
    auto const& global = relevant_global_object(*this);

    // * a Window object whose associated Document is fully active, or
    if (auto* window = as_if<Window>(global))
        return window->associated_document().is_fully_active();

    // * a WorkerGlobalScope object whose closing flag is false and is not suspendable.
    // FIXME: Suspendable worker
    if (auto* worker_global_scope = as_if<WorkerGlobalScope>(global)) {
        return !worker_global_scope->is_closing();
    }

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
    auto source_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(relevant_settings_object(*this));

    BroadcastChannelMessage message_to_send {
        .storage_key = source_storage_key,
        .channel_name = name().to_string(),
        .source_origin = source_origin,
        .serialized_message = serialized,
        .source_process_id = Core::System::getpid(),
        .source_channel_id = m_channel_id,
    };

    // Steps 6-9.
    deliver_message_locally(message_to_send);

    // NB: Other WebContent processes receive this via the browser-process IPC fanout.
    //     Child worker processes are not part of that routing path, so forward to them directly here.
    Bindings::principal_host_defined_page(realm()).client().page_did_post_broadcast_channel_message(message_to_send);

    WebWorkerClient::for_each_client([&](WebWorkerClient& client) {
        client.async_broadcast_channel_message(message_to_send);
        return IterationDecision::Continue;
    });

    return {};
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-postmessage
void BroadcastChannel::deliver_message_locally(BroadcastChannelMessage const& message)
{
    auto& vm = Bindings::main_thread_vm();

    // 6. Let destinations be a list of BroadcastChannel objects that match the following criteria:
    GC::RootVector<GC::Ref<BroadcastChannel>> destinations(vm.heap());

    // * The result of running obtain a storage key for non-storage purposes with their relevant settings object equals sourceStorageKey.
    auto same_origin_broadcast_channels = s_broadcast_channel_repository.registered_channels_for_key(message.storage_key);
    for (auto const& channel : same_origin_broadcast_channels) {
        // * They are eligible for messaging.
        if (!channel->is_eligible_for_messaging())
            continue;

        // * Their channel name is this's channel name.
        if (channel->name() != message.channel_name)
            continue;

        destinations.append(*channel);
    }

    // 7. Remove source from destinations.
    destinations.remove_all_matching([&](auto destination) {
        return message.source_process_id == Core::System::getpid() && destination->m_channel_id == message.source_channel_id;
    });

    // FIXME: 8. Sort destinations such that all BroadcastChannel objects whose relevant agents are the same are sorted in creation order, oldest first.
    //    (This does not define a complete ordering. Within this constraint, user agents may sort the list in any implementation-defined manner.)

    // 9. For each destination in destinations, queue a global task on the DOM manipulation task source given destination's relevant global object to perform the following steps:
    for (auto destination : destinations) {
        HTML::queue_global_task(HTML::Task::Source::DOMManipulation, relevant_global_object(destination), GC::create_function(vm.heap(), [&vm, destination, message] {
            // 1. If destination's closed flag is true, then abort these steps.
            if (destination->m_closed_flag)
                return;

            // 2. Let targetRealm be destination's relevant realm.
            auto& target_realm = relevant_realm(destination);

            // 3. Let data be StructuredDeserialize(serialized, targetRealm).
            //    If this throws an exception, catch it, fire an event named messageerror at destination, using MessageEvent, with its
            //    origin initialized to sourceOrigin, and then abort these steps.
            auto data_or_error = structured_deserialize(vm, message.serialized_message, target_realm);
            if (data_or_error.is_exception()) {
                MessageEventInit event_init {};
                event_init.origin = message.source_origin.serialize();
                auto event = MessageEvent::create(target_realm, HTML::EventNames::messageerror, event_init);
                event->set_is_trusted(true);
                destination->dispatch_event(event);
                return;
            }

            // 4. Fire an event named message at destination, using MessageEvent, with the data attribute initialized to data and
            //    its origin initialized to sourceOrigin.
            MessageEventInit event_init {};
            event_init.data = data_or_error.release_value();
            event_init.origin = message.source_origin.serialize();
            auto event = MessageEvent::create(target_realm, HTML::EventNames::message, event_init);
            event->set_is_trusted(true);
            destination->dispatch_event(event);
        }));
    }
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
