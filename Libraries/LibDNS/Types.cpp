/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDNS/Types.h>
#include <RustFFI.h>

namespace DNS::Messages {

static void store_string(void* context, FFI::FfiBytes bytes)
{
    *static_cast<String*>(context) = MUST(String::from_utf8({ reinterpret_cast<char const*>(bytes.data), bytes.length }));
}

String to_string(ResourceType type)
{
    String name;
    FFI::dns_resource_type_name(to_underlying(type), store_string, &name);
    return name;
}

Optional<ResourceType> resource_type_from_string(StringView name)
{
    u16 type = 0;
    if (!FFI::dns_resource_type_from_name({ reinterpret_cast<u8 const*>(name.characters_without_null_termination()), name.length() }, &type))
        return {};
    return static_cast<ResourceType>(type);
}

}
