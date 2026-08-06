/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/Actors/ObjectActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<ObjectActor> ObjectActor::create(DevToolsServer& devtools, String name, WeakPtr<ThreadActor> thread, u64 object_id, String class_name)
{
    return adopt_ref(*new ObjectActor(devtools, move(name), move(thread), object_id, move(class_name)));
}

ObjectActor::ObjectActor(DevToolsServer& devtools, String name, WeakPtr<ThreadActor> thread, u64 object_id, String class_name)
    : Actor(devtools, move(name))
    , m_thread(move(thread))
    , m_object_id(object_id)
    , m_class_name(move(class_name))
{
}

ObjectActor::~ObjectActor() = default;

JsonObject ObjectActor::grip() const
{
    JsonObject grip;
    grip.set("type"sv, "object"sv);
    grip.set("actor"sv, name());
    grip.set("class"sv, m_class_name);
    grip.set("extensible"sv, true);
    grip.set("frozen"sv, false);
    grip.set("sealed"sv, false);
    return grip;
}

void ObjectActor::handle_message(Message const& message)
{
    if (message.type == "release"sv) {
        send_response(message, {});
        if (auto thread = m_thread.strong_ref()) {
            thread->release_pause_actor(*this);
        } else {
            devtools().unregister_actor(name());
        }
        return;
    }

    auto thread = m_thread.strong_ref();
    if (!thread) {
        send_unknown_actor_error(message, name());
        return;
    }

    if (message.type == "enumProperties"sv) {
        thread->get_object_properties(*this, message, ThreadActor::ObjectPropertiesRequest::Iterator);
        return;
    }

    if (message.type == "enumSymbols"sv) {
        thread->get_object_symbols(*this, message);
        return;
    }

    if (message.type == "prototypeAndProperties"sv) {
        thread->get_object_properties(*this, message, ThreadActor::ObjectPropertiesRequest::PrototypeAndProperties);
        return;
    }

    if (message.type == "prototype"sv) {
        thread->get_object_properties(*this, message, ThreadActor::ObjectPropertiesRequest::Prototype);
        return;
    }

    if (message.type == "property"sv) {
        auto name = get_required_parameter<String>(message, "name"sv);
        if (!name.has_value())
            return;
        thread->get_object_properties(*this, message, ThreadActor::ObjectPropertiesRequest::Property, name.release_value());
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
