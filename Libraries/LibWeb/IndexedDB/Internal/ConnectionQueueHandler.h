/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/IndexedDB/Internal/RequestList.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#connection-queues
class ConnectionQueueHandler {
    struct Connection;

public:
    static RequestList& for_key_and_name(StorageAPI::StorageKey const& key, Utf16String const& name);
    static ConnectionQueueHandler& the()
    {
        static ConnectionQueueHandler& instance = *new ConnectionQueueHandler;
        return instance;
    }

private:
    Vector<NonnullRefPtr<Connection>> m_open_requests;

    struct Connection final : public RefCounted<Connection> {
        Connection(StorageAPI::StorageKey storage_key, Utf16String name)
            : storage_key(move(storage_key))
            , name(move(name))
        {
        }

        StorageAPI::StorageKey storage_key;
        Utf16String name;
        RequestList request_list;
    };
};

}
