/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

template<typename PropertyType = ByteString>
static ErrorOr<PropertyType, WebDriver::Error> get_property(JsonValue const& payload, StringView key)
{
    if (!payload.is_object())
        return WebDriver::Error::from_code(ErrorCode::InvalidArgument, "Payload is not a JSON object");

    auto property = payload.as_object().get(key);

    if (!property.has_value())
        return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("No property called '{}' present", key));

    if constexpr (IsSame<PropertyType, ByteString>) {
        if (!property->is_string())
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a String", key));
        return property->as_string();
    } else if constexpr (IsSame<PropertyType, bool>) {
        if (!property->is_bool())
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a Boolean", key));
        return property->as_bool();
    } else if constexpr (IsSame<PropertyType, u32>) {
        if (auto maybe_u32 = property->get_u32(); maybe_u32.has_value())
            return *maybe_u32;
        return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a Number", key));
    } else if constexpr (IsSame<PropertyType, JsonArray const*>) {
        if (!property->is_array())
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not an Array", key));
        return &property->as_array();
    } else if constexpr (IsSame<PropertyType, JsonObject const*>) {
        if (!property->is_object())
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not an Object", key));
        return &property->as_object();
    } else {
        static_assert(DependentFalse<PropertyType>, "get_property invoked with unknown property type");
        VERIFY_NOT_REACHED();
    }
}

}
