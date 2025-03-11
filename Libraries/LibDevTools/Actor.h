/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
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
    virtual ~Actor();

    String const& name() const { return m_name; }
    virtual void handle_message(StringView type, JsonObject const&) = 0;

    class [[nodiscard]] BlockToken {
    public:
        BlockToken(Badge<Actor>, Actor&);
        ~BlockToken();

        BlockToken(BlockToken const&) = delete;
        BlockToken& operator=(BlockToken const&) = delete;

        BlockToken(BlockToken&&);
        BlockToken& operator=(BlockToken&&);

    private:
        WeakPtr<Actor> m_actor;
    };

    void send_message(JsonObject, Optional<BlockToken> block_token = {});
    void send_missing_parameter_error(StringView parameter);
    void send_unrecognized_packet_type_error(StringView type);
    void send_unknown_actor_error(StringView actor);

protected:
    explicit Actor(DevToolsServer&, String name);

    DevToolsServer& devtools() { return m_devtools; }
    DevToolsServer const& devtools() const { return m_devtools; }

    BlockToken block_responses();

    template<typename ParameterType>
    auto get_required_parameter(JsonObject const& message, StringView parameter)
    {
        auto result = [&]() {
            if constexpr (IsIntegral<ParameterType>)
                return message.get_integer<ParameterType>(parameter);
            else if constexpr (IsSame<ParameterType, bool>)
                return message.get_bool(parameter);
            else if constexpr (IsSame<ParameterType, String>)
                return message.get_string(parameter);
            else if constexpr (IsSame<ParameterType, JsonObject>)
                return message.get_object(parameter);
            else if constexpr (IsSame<ParameterType, JsonArray>)
                return message.get_array(parameter);
            else
                static_assert(DependentFalse<ParameterType>);
        }();

        if (!result.has_value())
            send_missing_parameter_error(parameter);

        return result;
    }

    template<typename ActorType = Actor, typename Handler>
    auto async_handler(Handler&& handler)
    {
        return [weak_self = make_weak_ptr<ActorType>(), handler = forward<Handler>(handler), block_token = block_responses()](auto result) mutable {
            if (result.is_error()) {
                dbgln_if(DEVTOOLS_DEBUG, "Error performing async action: {}", result.error());
                return;
            }

            if (auto self = weak_self.strong_ref()) {
                JsonObject response;
                handler(*self, result.release_value(), response);
                self->send_message(move(response), move(block_token));
            }
        };
    }

    auto default_async_handler()
    {
        return async_handler([](auto&, auto, auto) { });
    }

private:
    DevToolsServer& m_devtools;
    String m_name;

    Vector<JsonObject> m_blocked_responses;
    bool m_block_responses { false };
};

}
