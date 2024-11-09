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
#include <LibJS/Runtime/Value.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

template<typename PropertyType = ByteString>
static ErrorOr<PropertyType, WebDriver::Error> get_property(JsonObject const& payload, StringView key)
{
    auto property = payload.get(key);

    if (!property.has_value())
        return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("No property called '{}' present", key));

    auto is_safe_number = []<typename T>(T value) {
        if constexpr (sizeof(T) >= 8) {
            if (value > static_cast<T>(JS::MAX_ARRAY_LIKE_INDEX))
                return false;

            if constexpr (IsSigned<T>) {
                if (value < -static_cast<T>(JS::MAX_ARRAY_LIKE_INDEX))
                    return false;
            }
        }

        return true;
    };

    if constexpr (IsSame<PropertyType, ByteString>) {
        if (!property->is_string())
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a String", key));
        return property->as_string();
    } else if constexpr (IsSame<PropertyType, bool>) {
        if (!property->is_bool())
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a Boolean", key));
        return property->as_bool();
    } else if constexpr (IsIntegral<PropertyType>) {
        if (auto maybe_number = property->get_integer<PropertyType>(); maybe_number.has_value() && is_safe_number(*maybe_number))
            return *maybe_number;
        return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not an Integer", key));
    } else if constexpr (IsSame<PropertyType, double>) {
        if (auto maybe_number = property->get_double_with_precision_loss(); maybe_number.has_value() && is_safe_number(*maybe_number))
            return *maybe_number;
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

template<typename PropertyType = ByteString>
static ErrorOr<PropertyType, WebDriver::Error> get_property(JsonValue const& payload, StringView key)
{
    if (!payload.is_object())
        return WebDriver::Error::from_code(ErrorCode::InvalidArgument, "Payload is not a JSON object");
    return get_property<PropertyType>(payload.as_object(), key);
}

template<typename PropertyType = ByteString>
static ErrorOr<Optional<PropertyType>, WebDriver::Error> get_optional_property(JsonObject const& object, StringView key)
{
    if (!object.has(key))
        return OptionalNone {};
    return get_property<PropertyType>(object, key);
}

template<Arithmetic PropertyType>
static ErrorOr<PropertyType, WebDriver::Error> get_property_with_limits(JsonObject const& object, StringView key, Optional<PropertyType> min, Optional<PropertyType> max)
{
    auto value = TRY(get_property<PropertyType>(object, key));

    if (min.has_value() && value < *min)
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' must not be less than {}", key, *min));
    if (max.has_value() && value > *max)
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' must not be greater than {}", key, *max));

    return value;
}

template<Arithmetic PropertyType>
static ErrorOr<Optional<PropertyType>, WebDriver::Error> get_optional_property_with_limits(JsonObject const& object, StringView key, Optional<PropertyType> min, Optional<PropertyType> max)
{
    if (!object.has(key))
        return OptionalNone {};
    return get_property_with_limits<PropertyType>(object, key, min, max);
}

}
