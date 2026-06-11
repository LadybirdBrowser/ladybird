/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API CookiesActor final : public Actor {
public:
    static constexpr auto base_name = "cookies"sv;

    static NonnullRefPtr<CookiesActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~CookiesActor() override;

    Optional<String> host() const;
    JsonObject serialize_storage() const;

private:
    CookiesActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    void get_fields(Message const&);
    void get_store_objects(Message const&);

    WeakPtr<TabActor> m_tab;
};

}
