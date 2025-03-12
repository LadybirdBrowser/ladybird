/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class ThreadActor final : public Actor {
public:
    static constexpr auto base_name = "thread"sv;

    static NonnullRefPtr<ThreadActor> create(DevToolsServer&, String name);
    virtual ~ThreadActor() override;

private:
    ThreadActor(DevToolsServer&, String name);

    virtual void handle_message(Message const&) override;
};

}
