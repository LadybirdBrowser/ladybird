/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibCore/Proxy.h>
#include <LibURL/URL.h>

namespace Web {

class ProxyMappings {
public:
    static ProxyMappings& the();

    Core::ProxyData proxy_for_url(URL::URL const&) const;
    void set_mappings(Vector<ByteString> proxies, OrderedHashMap<ByteString, size_t> mappings);

private:
    ProxyMappings() = default;
    ~ProxyMappings() = default;

    Vector<ByteString> m_proxies;
    OrderedHashMap<ByteString, size_t> m_mappings;
};

}
