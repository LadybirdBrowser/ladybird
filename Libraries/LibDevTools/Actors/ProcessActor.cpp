/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/ProcessActor.h>

namespace DevTools {

NonnullRefPtr<ProcessActor> ProcessActor::create(DevToolsServer& devtools, String name, ProcessDescription description)
{
    return adopt_ref(*new ProcessActor(devtools, move(name), move(description)));
}

ProcessActor::ProcessActor(DevToolsServer& devtools, String name, ProcessDescription description)
    : Actor(devtools, move(name))
    , m_description(move(description))
{
}

ProcessActor::~ProcessActor() = default;

void ProcessActor::handle_message(Message const& message)
{
    send_unrecognized_packet_type_error(message);
}

JsonObject ProcessActor::serialize_description() const
{
    JsonObject traits;
    traits.set("watcher"_sv, m_description.is_parent);
    traits.set("supportsReloadDescriptor"_sv, true);

    JsonObject description;
    description.set("actor"_sv, name());
    description.set("id"_sv, m_description.id);
    description.set("isParent"_sv, m_description.is_parent);
    description.set("isWindowlessParent"_sv, m_description.is_windowless_parent);
    description.set("traits"_sv, move(traits));
    return description;
}

}
