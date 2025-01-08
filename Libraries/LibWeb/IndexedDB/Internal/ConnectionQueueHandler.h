/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/IndexedDB/Internal/RequestList.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

using ConnectionMap = HashMap<StorageAPI::StorageKey, HashMap<String, RequestList>>;

// https://w3c.github.io/IndexedDB/#connection-queues
class ConnectionQueueHandler {
public:
    static RequestList& for_key_and_name(StorageAPI::StorageKey& key, String& name);
    static ConnectionQueueHandler& the()
    {
        static ConnectionQueueHandler s_instance;
        return s_instance;
    }

private:
    ConnectionMap m_open_requests;
};

}
