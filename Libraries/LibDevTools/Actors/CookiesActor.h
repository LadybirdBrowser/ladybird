/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>
#include <LibHTTP/Forward.h>

namespace DevTools {

class DEVTOOLS_API CookiesActor final : public Actor {
public:
    static constexpr auto base_name = "cookies"sv;

    static NonnullRefPtr<CookiesActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~CookiesActor() override;

    Optional<String> host() const;
    JsonObject serialize_storage() const;
    void on_cookies_changed(Vector<HTTP::Cookie::Cookie>);

private:
    CookiesActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    void get_fields(Message const&);
    void get_store_objects(Message const&);
    void edit_item(Message const&);
    void add_item(Message const&);
    void remove_item(Message const&);
    void remove_all(Message const&);
    void remove_all_session_cookies(Message const&);
    HashTable<String> visible_cookie_unique_keys(String const& host) const;
    void send_cookie_store_update(String const& host, JsonArray added, JsonArray changed, JsonArray deleted);

    WeakPtr<TabActor> m_tab;
    HashMap<String, HashTable<String>> m_visible_cookie_unique_keys;
};

}
