/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Debug.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class Actor
    : public RefCounted<Actor>
    , public Weakable<Actor> {
public:
    struct Message {
        StringView type {};
        JsonObject data {};
        u64 id { 0 };
    };

    virtual ~Actor();

    String const& name() const { return m_name; }

    void message_received(StringView type, JsonObject);

    // Use send_response when replying directly to a request received from the client.
    void send_response(Message const&, JsonObject);

    // Use send_message when sending an unprompted message to the client.
    void send_message(JsonObject);

    void send_missing_parameter_error(Optional<Message const&>, StringView parameter);
    void send_unrecognized_packet_type_error(Message const&);
    void send_unknown_actor_error(Optional<Message const&>, StringView actor);

protected:
    explicit Actor(DevToolsServer&, String name);

    virtual void handle_message(Message const&) = 0;

    DevToolsServer& devtools() { return m_devtools; }
    DevToolsServer const& devtools() const { return m_devtools; }

    template<typename ParameterType>
    auto get_required_parameter(Message const& message, StringView parameter)
    {
        auto result = [&]() {
            if constexpr (IsIntegral<ParameterType>)
                return message.data.get_integer<ParameterType>(parameter);
            else if constexpr (IsSame<ParameterType, bool>)
                return message.data.get_bool(parameter);
            else if constexpr (IsSame<ParameterType, String>)
                return message.data.get_string(parameter);
            else if constexpr (IsSame<ParameterType, JsonObject>)
                return message.data.get_object(parameter);
            else if constexpr (IsSame<ParameterType, JsonArray>)
                return message.data.get_array(parameter);
            else
                static_assert(DependentFalse<ParameterType>);
        }();

        if (!result.has_value())
            send_missing_parameter_error(message, parameter);

        return result;
    }

    template<typename ActorType = Actor, typename Handler>
    auto async_handler(Optional<Message const&> message, Handler&& handler)
    {
        auto message_id = message.map([](auto const& message) { return message.id; });

        return [weak_self = make_weak_ptr<ActorType>(), message_id, handler = forward<Handler>(handler)](auto result) mutable {
            if (result.is_error()) {
                dbgln_if(DEVTOOLS_DEBUG, "Error performing async action: {}", result.error());
                return;
            }

            if (auto self = weak_self.strong_ref()) {
                JsonObject response;
                handler(*self, result.release_value(), response);

                if (message_id.has_value())
                    self->send_response({ .id = *message_id }, move(response));
                else
                    self->send_message(move(response));
            }
        };
    }

    auto default_async_handler(Message const& message)
    {
        return async_handler(message, [](auto&, auto, auto) { });
    }

private:
    DevToolsServer& m_devtools;
    String m_name;

    struct PendingResponse {
        Optional<u64> id;
        Optional<JsonObject> response;
    };
    Vector<PendingResponse, 1> m_pending_responses;
    u64 m_next_message_id { 0 };
};

}
