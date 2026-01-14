/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/NetworkParentActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<NetworkParentActor> NetworkParentActor::create(DevToolsServer& devtools, String name)
{
    return adopt_ref(*new NetworkParentActor(devtools, move(name)));
}

NetworkParentActor::NetworkParentActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

NetworkParentActor::~NetworkParentActor() = default;

void NetworkParentActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "setPersist"sv) {
        // FIXME: Implement persist functionality
        send_response(message, move(response));
        return;
    }

    if (message.type == "setNetworkThrottling"sv) {
        // FIXME: Implement network throttling
        send_response(message, move(response));
        return;
    }

    if (message.type == "getNetworkThrottling"sv) {
        response.set("state"sv, JsonValue {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "clearNetworkThrottling"sv) {
        send_response(message, move(response));
        return;
    }

    if (message.type == "setSaveRequestAndResponseBodies"sv) {
        // FIXME: Implement saving request/response bodies
        send_response(message, move(response));
        return;
    }

    if (message.type == "setBlockedUrls"sv) {
        // FIXME: Implement URL blocking
        send_response(message, move(response));
        return;
    }

    if (message.type == "getBlockedUrls"sv) {
        response.set("urls"sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "blockRequest"sv) {
        // FIXME: Implement request blocking
        send_response(message, move(response));
        return;
    }

    if (message.type == "unblockRequest"sv) {
        // FIXME: Implement request unblocking
        send_response(message, move(response));
        return;
    }

    if (message.type == "override"sv) {
        // FIXME: Implement request override
        send_response(message, move(response));
        return;
    }

    if (message.type == "removeOverride"sv) {
        // FIXME: Implement remove override
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
