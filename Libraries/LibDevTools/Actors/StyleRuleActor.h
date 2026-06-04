/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class DEVTOOLS_API StyleRuleActor final : public Actor {
public:
    static constexpr auto base_name = "style-rule"sv;

    static NonnullRefPtr<StyleRuleActor> create(DevToolsServer&, String name, JsonObject rule);
    virtual ~StyleRuleActor() override;

    JsonObject serialize_rule() const;

private:
    StyleRuleActor(DevToolsServer&, String name, JsonObject rule);

    virtual void handle_message(Message const&) override;

    JsonObject m_rule;
};

}
