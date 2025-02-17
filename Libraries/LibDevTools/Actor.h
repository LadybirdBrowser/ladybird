/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteString.h>
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

    void send_message(JsonValue, Optional<BlockToken> block_token = {});
    void send_missing_parameter_error(StringView parameter);
    void send_unrecognized_packet_type_error(StringView type);
    void send_unknown_actor_error(StringView actor);

protected:
    explicit Actor(DevToolsServer&, ByteString name);

    DevToolsServer& devtools() { return m_devtools; }
    DevToolsServer const& devtools() const { return m_devtools; }

    BlockToken block_responses();

private:
    DevToolsServer& m_devtools;
    String m_name;

    Vector<JsonValue> m_blocked_responses;
    bool m_block_responses { false };
};

}
