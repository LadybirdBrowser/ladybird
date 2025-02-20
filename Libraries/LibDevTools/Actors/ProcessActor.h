/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

struct ProcessDescription {
    u64 id { 0 };
    bool is_parent { false };
    bool is_windowless_parent { false };
};

class ProcessActor final : public Actor {
public:
    static constexpr auto base_name = "process"sv;

    static NonnullRefPtr<ProcessActor> create(DevToolsServer&, String name, ProcessDescription);
    virtual ~ProcessActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

    ProcessDescription const& description() const { return m_description; }
    JsonObject serialize_description() const;

private:
    ProcessActor(DevToolsServer&, String name, ProcessDescription);

    ProcessDescription m_description;
};

}
