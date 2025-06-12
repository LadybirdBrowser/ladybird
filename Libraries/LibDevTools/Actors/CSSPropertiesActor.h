/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

struct CSSProperty {
    String name;
    bool is_inherited { false };
};

class CSSPropertiesActor final : public Actor {
public:
    static constexpr auto base_name = "css-properties"sv;

    static NonnullRefPtr<CSSPropertiesActor> create(DevToolsServer&, String name);
    virtual ~CSSPropertiesActor() override;

private:
    CSSPropertiesActor(DevToolsServer&, String name);

    virtual void handle_message(Message const&) override;
};

}
