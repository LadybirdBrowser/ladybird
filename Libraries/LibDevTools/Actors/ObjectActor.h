/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>

namespace DevTools {

class ObjectActor final : public Actor {
public:
    static constexpr auto base_name = "object"sv;

    static NonnullRefPtr<ObjectActor> create(DevToolsServer&, String name, WeakPtr<ThreadActor>, u64 object_id, String class_name);
    virtual ~ObjectActor() override;

    JsonObject grip() const;
    u64 object_id() const { return m_object_id; }

private:
    ObjectActor(DevToolsServer&, String name, WeakPtr<ThreadActor>, u64 object_id, String class_name);

    virtual void handle_message(Message const&) override;

    WeakPtr<ThreadActor> m_thread;
    u64 m_object_id { 0 };
    String m_class_name;
};

}
