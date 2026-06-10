/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonArray.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/Forward.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>

namespace DevTools {

class DEVTOOLS_API StorageActor final : public Actor {
public:
    static constexpr auto base_name = "storage"sv;

    static NonnullRefPtr<StorageActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, Web::StorageAPI::StorageEndpointType);
    virtual ~StorageActor() override;

    StringView resource_type() const;
    StringView resource_key() const;
    Optional<String> host() const;
    JsonObject serialize_storage() const;

private:
    StorageActor(DevToolsServer&, String name, WeakPtr<TabActor>, Web::StorageAPI::StorageEndpointType);

    virtual void handle_message(Message const&) override;

    void get_fields(Message const&);
    void get_store_objects(Message const&);
    void send_store_objects(Message const&, Optional<String> requested_host, Optional<JsonArray> requested_names, JsonObject options, ErrorOr<Vector<DevToolsDelegate::StorageItem>>);

    WeakPtr<TabActor> m_tab;
    Web::StorageAPI::StorageEndpointType m_storage_endpoint;
};

}
