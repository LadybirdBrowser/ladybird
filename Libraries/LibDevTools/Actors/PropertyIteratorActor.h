/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class PropertyIteratorActor final : public Actor {
public:
    struct Property {
        String name;
        JsonObject descriptor;
    };

    static constexpr auto base_name = "propertyIterator"sv;

    static NonnullRefPtr<PropertyIteratorActor> create(DevToolsServer&, String name, WeakPtr<ThreadActor>, Vector<Property> properties);
    virtual ~PropertyIteratorActor() override;

    JsonObject form() const;

private:
    PropertyIteratorActor(DevToolsServer&, String name, WeakPtr<ThreadActor>, Vector<Property> properties);

    virtual void handle_message(Message const&) override;
    JsonObject serialize_properties(size_t start, size_t count) const;

    WeakPtr<ThreadActor> m_thread;
    Vector<Property> m_properties;
};

}
