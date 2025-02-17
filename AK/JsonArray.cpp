/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonArraySerializer.h>
#include <AK/StringBuilder.h>

namespace AK {

String JsonArray::serialized() const
{
    StringBuilder builder;
    serialize(builder);

    return MUST(builder.to_string());
}

void JsonArray::serialize(StringBuilder& builder) const
{
    auto serializer = MUST(JsonArraySerializer<>::try_create(builder));
    for_each([&](auto const& value) {
        MUST(serializer.add(value));
    });
    MUST(serializer.finish());
}

}
