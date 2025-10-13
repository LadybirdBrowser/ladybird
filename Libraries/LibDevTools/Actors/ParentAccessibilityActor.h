/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API ParentAccessibilityActor final : public Actor {
public:
    static constexpr auto base_name = "parent-accessibility"sv;

    static NonnullRefPtr<ParentAccessibilityActor> create(DevToolsServer&, String name);
    virtual ~ParentAccessibilityActor() override;

private:
    ParentAccessibilityActor(DevToolsServer&, String name);

    virtual void handle_message(Message const&) override;
};

}
