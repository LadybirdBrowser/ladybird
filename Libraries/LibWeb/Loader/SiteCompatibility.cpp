/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Loader/SiteCompatibility.h>

namespace Web {

static ErrorOr<Vector<String>> parse_patterns(JsonObject const& object)
{
    auto patterns = object.get_array("matches"sv);
    if (!patterns.has_value() || patterns->is_empty())
        return Error::from_string_literal("Site-compatibility rule must contain a non-empty 'matches' array");

    Vector<String> result;
    TRY(result.try_ensure_capacity(patterns->size()));
    for (size_t i = 0; i < patterns->size(); ++i) {
        auto const& pattern = patterns->at(i);
        if (!pattern.is_string() || pattern.as_string().is_empty())
            return Error::from_string_literal("Site-compatibility rule patterns must be non-empty strings");
        TRY(result.try_append(pattern.as_string()));
    }
    return result;
}

static ErrorOr<Vector<UserAgentTransformation>> parse_user_agent_transformations(JsonObject const& object)
{
    if (!object.has("user_agent"sv))
        return Vector<UserAgentTransformation> {};

    // A present key must hold a non-empty array: JsonObject::get_array() alone can't tell a missing key from a value
    // of the wrong type, and a rule with a broken half must fail, not run with the half that parsed.
    auto transformations = object.get_array("user_agent"sv);
    if (!transformations.has_value() || transformations->is_empty())
        return Error::from_string_literal("Site-compatibility rule 'user_agent' must be a non-empty array");

    Vector<UserAgentTransformation> result;
    TRY(result.try_ensure_capacity(transformations->size()));
    for (size_t i = 0; i < transformations->size(); ++i) {
        auto const& transformation = transformations->at(i);
        if (!transformation.is_string())
            return Error::from_string_literal("User-Agent transformations must be strings");
        if (transformation.as_string() == "hide_Ladybird"sv)
            TRY(result.try_append(UserAgentTransformation::HideLadybird));
        else
            return Error::from_string_literal("Unknown User-Agent transformation");
    }
    return result;
}

// The names are the ones the IDL bindings gate behind [Experimental]: an interface name for a constructor the global
// would otherwise hide, or Interface.member for an attribute or operation. A name nothing gates simply never matches.
static ErrorOr<Vector<String>> parse_exposed_interfaces(JsonObject const& object)
{
    if (!object.has("expose_experimental_interfaces"sv))
        return Vector<String> {};

    auto names = object.get_array("expose_experimental_interfaces"sv);
    if (!names.has_value() || names->is_empty())
        return Error::from_string_literal("Site-compatibility rule 'expose_experimental_interfaces' must be a non-empty array");

    Vector<String> result;
    TRY(result.try_ensure_capacity(names->size()));

    for (size_t i = 0; i < names->size(); ++i) {
        auto const& name = names->at(i);
        if (!name.is_string() || name.as_string().is_empty())
            return Error::from_string_literal("Site-compatibility rule 'expose_experimental_interfaces' entries must be non-empty strings");
        TRY(result.try_append(name.as_string()));
    }

    return result;
}

ErrorOr<SiteCompatibilityRule> SiteCompatibilityRule::from_json(JsonValue const& value)
{
    if (!value.is_object())
        return Error::from_string_literal("Site-compatibility rule must be a JSON object");

    auto const& object = value.as_object();
    auto transformations = TRY(parse_user_agent_transformations(object));
    auto exposed_interfaces = TRY(parse_exposed_interfaces(object));
    if (transformations.is_empty() && exposed_interfaces.is_empty())
        return Error::from_string_literal("Site-compatibility rule must contain a 'user_agent' or an 'expose_experimental_interfaces' array");

    return create(TRY(parse_patterns(object)), move(transformations), move(exposed_interfaces));
}

ErrorOr<SiteCompatibilityRule> SiteCompatibilityRule::create(Vector<String> patterns, Vector<UserAgentTransformation> transformations, Vector<String> exposed_interfaces)
{
    Vector<URL::RustIntegration::URLPattern> compiled_patterns;
    TRY(compiled_patterns.try_ensure_capacity(patterns.size()));

    for (auto const& pattern : patterns) {
        auto compiled_pattern = URL::RustIntegration::URLPattern::create(pattern);
        if (compiled_pattern.is_error())
            return Error::from_string_literal("Site-compatibility rule contains an invalid URL pattern");
        TRY(compiled_patterns.try_append(compiled_pattern.release_value()));
    }

    return SiteCompatibilityRule { move(patterns), move(compiled_patterns), move(transformations), move(exposed_interfaces) };
}

SiteCompatibilityRule::SiteCompatibilityRule(Vector<String> patterns, Vector<URL::RustIntegration::URLPattern> compiled_patterns, Vector<UserAgentTransformation> transformations, Vector<String> exposed_interfaces)
    : m_patterns(move(patterns))
    , m_compiled_patterns(move(compiled_patterns))
    , m_user_agent_transformations(move(transformations))
    , m_exposed_interfaces(move(exposed_interfaces))
{
}

bool SiteCompatibilityRule::exposes_experimental_interface(StringView name) const
{
    return m_exposed_interfaces.contains_slow(name);
}

bool SiteCompatibilityRule::matches(URL::URL const& url) const
{
    auto serialized_url = url.serialize();
    for (auto const& pattern : m_compiled_patterns) {
        auto result = pattern.match(serialized_url, {});
        if (!result.is_error() && result.value().has_value())
            return true;
    }
    return false;
}

static String remove_ladybird_product(StringView user_agent)
{
    StringBuilder builder;
    bool first = true;
    for (auto product : user_agent.split_view(' ')) {
        if (product.starts_with("Ladybird/"sv))
            continue;
        if (!first)
            builder.append(' ');
        builder.append(product);
        first = false;
    }
    return MUST(builder.to_string());
}

String SiteCompatibilityRule::apply_user_agent_transformations(StringView user_agent) const
{
    auto transformed_user_agent = MUST(String::from_utf8(user_agent));
    for (auto transformation : m_user_agent_transformations) {
        switch (transformation) {
        case UserAgentTransformation::HideLadybird:
            transformed_user_agent = remove_ladybird_product(transformed_user_agent);
            break;
        }
    }
    return transformed_user_agent;
}

String SiteCompatibilityData::user_agent_for_url(URL::URL const& url, StringView default_user_agent) const
{
    auto user_agent = MUST(String::from_utf8(default_user_agent));
    for (auto const& rule : m_rules) {
        if (rule.matches(url))
            user_agent = rule.apply_user_agent_transformations(user_agent);
    }
    return user_agent;
}

String SiteCompatibilityData::user_agent_for_websocket_url(URL::URL const& url, StringView default_user_agent) const
{
    auto http_url = url;
    if (http_url.scheme() == "ws"sv)
        http_url.set_scheme("http"_string);
    else if (http_url.scheme() == "wss"sv)
        http_url.set_scheme("https"_string);
    return user_agent_for_url(http_url, default_user_agent);
}

bool SiteCompatibilityData::exposes_experimental_interface(URL::URL const& url, StringView name) const
{
    for (auto const& rule : m_rules) {
        if (rule.exposes_experimental_interface(name) && rule.matches(url))
            return true;
    }
    return false;
}

ErrorOr<SiteCompatibilityData> SiteCompatibilityData::from_json(JsonValue const& value)
{
    if (!value.is_array())
        return Error::from_string_literal("Site-compatibility data must be a JSON array");

    SiteCompatibilityData data;
    auto const& rules = value.as_array();
    for (size_t i = 0; i < rules.size(); ++i)
        data.add_rule(TRY(SiteCompatibilityRule::from_json(rules.at(i))));
    return data;
}

}
