/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/IPv4Address.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Export.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

namespace Core {

// FIXME: Username/password support.
struct CORE_API ProxyData {
    enum Type {
        Direct,
        SOCKS5,
        HTTP,
        HTTPS
    } type { Type::Direct };

    String host;
    u16 port { 0 };

    bool operator==(ProxyData const& other) const = default;

    static constexpr StringView type_name(Type type)
    {
        switch (type) {
        case Type::Direct:
            return "Direct"sv;
        case Type::SOCKS5:
            return "SOCKS5"sv;
        case Type::HTTP:
            return "HTTP"sv;
        case Type::HTTPS:
            return "HTTPS"sv;
        }
        VERIFY_NOT_REACHED();
    }

    bool is_direct() const { return type == Type::Direct; }

    static ErrorOr<ProxyData> parse_url(URL::URL const& url);
    static bool use_system_proxy();
    static Vector<ProxyData> get_proxies_for_url(URL::URL const& url);
};

}
