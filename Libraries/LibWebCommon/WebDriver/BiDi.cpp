/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibWebCommon/WebDriver/BiDi.h>

namespace Web::WebDriver {

// js-uint = 0..9007199254740991
Optional<u64> as_js_uint(JsonValue const& value)
{
    static constexpr u64 MAXIMUM_SAFE_INTEGER = 9007199254740991;

    if (!value.is_number())
        return {};

    return value.as_number().visit(
        [](u64 number) -> Optional<u64> {
            if (number > MAXIMUM_SAFE_INTEGER)
                return {};
            return number;
        },
        [](i64 number) -> Optional<u64> {
            if (number < 0 || static_cast<u64>(number) > MAXIMUM_SAFE_INTEGER)
                return {};
            return static_cast<u64>(number);
        },
        [](double number) -> Optional<u64> {
            if (!(number >= 0) || number > static_cast<double>(MAXIMUM_SAFE_INTEGER))
                return {};
            auto integer = static_cast<u64>(number);
            if (static_cast<double>(integer) != number)
                return {};
            return integer;
        });
}

// https://w3c.github.io/webdriver-bidi/#type-script-SerializationOptions
ErrorOr<SerializationOptions, Error> parse_serialization_options(JsonValue const& value)
{
    // script.SerializationOptions = {
    //   ? maxDomDepth: (js-uint / null) .default 0,
    //   ? maxObjectDepth: (js-uint / null) .default null,
    //   ? includeShadowTree: ("none" / "open" / "all") .default "none",
    // }
    if (!value.is_object())
        return Error::from_code(ErrorCode::InvalidArgument, "Serialization options must be an object"sv);

    auto const& object = value.as_object();
    SerializationOptions options;

    if (auto max_dom_depth = object.get("maxDomDepth"sv); max_dom_depth.has_value()) {
        if (max_dom_depth->is_null()) {
            options.max_dom_depth = {};
        } else {
            options.max_dom_depth = as_js_uint(*max_dom_depth);
            if (!options.max_dom_depth.has_value())
                return Error::from_code(ErrorCode::InvalidArgument, "maxDomDepth must be an unsigned integer or null"sv);
        }
    }

    if (auto max_object_depth = object.get("maxObjectDepth"sv); max_object_depth.has_value()) {
        if (!max_object_depth->is_null()) {
            options.max_object_depth = as_js_uint(*max_object_depth);
            if (!options.max_object_depth.has_value())
                return Error::from_code(ErrorCode::InvalidArgument, "maxObjectDepth must be an unsigned integer or null"sv);
        }
    }

    if (auto include_shadow_tree = object.get("includeShadowTree"sv); include_shadow_tree.has_value()) {
        auto string = include_shadow_tree->is_string() ? include_shadow_tree->as_string() : String {};
        if (string == "none"sv)
            options.include_shadow_tree = SerializationOptions::IncludeShadowTree::None;
        else if (string == "open"sv)
            options.include_shadow_tree = SerializationOptions::IncludeShadowTree::Open;
        else if (string == "all"sv)
            options.include_shadow_tree = SerializationOptions::IncludeShadowTree::All;
        else
            return Error::from_code(ErrorCode::InvalidArgument, "includeShadowTree must be \"none\", \"open\" or \"all\""sv);
    }

    return options;
}

}
