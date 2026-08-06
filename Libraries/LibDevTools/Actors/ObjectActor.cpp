/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibDevTools/Actors/ObjectActor.h>

namespace DevTools {

NonnullRefPtr<ObjectActor> ObjectActor::create(DevToolsServer& devtools, String name, u64 object_id, String class_name)
{
    return adopt_ref(*new ObjectActor(devtools, move(name), object_id, move(class_name)));
}

ObjectActor::ObjectActor(DevToolsServer& devtools, String name, u64 object_id, String class_name)
    : Actor(devtools, move(name))
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
    // FIXME: Support enumerating object properties.
    if (message.type == "prototypeAndProperties"sv) {
        JsonObject response;
        response.set("ownProperties"sv, JsonObject {});
        response.set("ownSymbols"sv, JsonArray {});
        response.set("safeGetterValues"sv, JsonObject {});
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
