/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class DEVTOOLS_API IndexedDBActor final : public Actor {
public:
    static constexpr auto base_name = "indexed-db"sv;

    static NonnullRefPtr<IndexedDBActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~IndexedDBActor() override;

    void get_storage_resource(Function<void(JsonObject)>);

private:
    IndexedDBActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    void get_fields(Message const&);
    void get_store_objects(Message const&);
    void remove_database(Message const&);
    void remove_all(Message const&);
    void remove_item(Message const&);
    void on_indexed_database_changed(JsonObject);

    JsonObject serialize_storage(JsonObject hosts) const;
    void send_inspection_error(Message const&, Error const&);
    void send_indexed_database_update(StringView update_type, String const& host, String const& name);
    void send_indexed_database_clear(String const& host, String const& name);

    WeakPtr<TabActor> m_tab;
    u64 m_indexed_database_change_listener_id { 0 };
};

}
