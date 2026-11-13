/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Loader/SiteCompatibility.h>

namespace Web {

// Only a site's own serialization is accepted, such as "https://example.com", so a rule can't name a subdomain, port or
// path that matching would ignore.
static ErrorOr<URL::Site> parse_site(StringView serialized_site)
{
    auto url = URL::Parser::basic_parse(serialized_site);
    if (!url.has_value())
        return Error::from_string_literal("Site-compatibility rule sites must be URLs");

    auto origin = url->origin();
    if (origin.is_opaque())
        return Error::from_string_literal("Site-compatibility rule sites must not be opaque");

    auto site = URL::Site::obtain(origin);
    if (site.serialize() != serialized_site)
        return Error::from_string_literal("Site-compatibility rule sites must be serialized sites, such as https://example.com");
    return site;
}

static ErrorOr<Vector<String>> parse_sites(JsonObject const& object)
{
    auto sites = object.get_array("sites"sv);
    if (!sites.has_value() || sites->is_empty())
        return Error::from_string_literal("Site-compatibility rule must contain a non-empty 'sites' array");

    Vector<String> result;
    TRY(result.try_ensure_capacity(sites->size()));
    for (size_t i = 0; i < sites->size(); ++i) {
        auto const& site = sites->at(i);
        if (!site.is_string())
            return Error::from_string_literal("Site-compatibility rule sites must be strings");
        TRY(result.try_append(site.as_string()));
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
        else if (transformation.as_string() == "hide_Chromium"sv)
            TRY(result.try_append(UserAgentTransformation::HideChromium));
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

    return create(TRY(parse_sites(object)), move(transformations), move(exposed_interfaces));
}

ErrorOr<SiteCompatibilityRule> SiteCompatibilityRule::create(Vector<String> serialized_sites, Vector<UserAgentTransformation> transformations, Vector<String> exposed_interfaces)
{
    Vector<URL::Site> sites;
    TRY(sites.try_ensure_capacity(serialized_sites.size()));
    for (auto const& serialized_site : serialized_sites)
        TRY(sites.try_append(TRY(parse_site(serialized_site))));

    return SiteCompatibilityRule { move(sites), move(transformations), move(exposed_interfaces) };
}

SiteCompatibilityRule::SiteCompatibilityRule(Vector<URL::Site> sites, Vector<UserAgentTransformation> transformations, Vector<String> exposed_interfaces)
    : m_sites(move(sites))
    , m_user_agent_transformations(move(transformations))
    , m_exposed_interfaces(move(exposed_interfaces))
{
}

bool SiteCompatibilityRule::exposes_experimental_interface(StringView name) const
{
    return m_exposed_interfaces.contains_slow(name);
}

bool SiteCompatibilityRule::matches(URL::Site const& site) const
{
    return m_sites.contains_slow(site);
}

static String remove_product(StringView user_agent, StringView name)
{
    StringBuilder builder;
    bool first = true;
    for (auto product : user_agent.split_view(' ')) {
        if (product.starts_with(name) && product.substring_view(name.length()).starts_with('/'))
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
            transformed_user_agent = remove_product(transformed_user_agent, "Ladybird"sv);
            break;
        case UserAgentTransformation::HideChromium:
            transformed_user_agent = remove_product(transformed_user_agent, "Chrome"sv);
            break;
        }
    }
    return transformed_user_agent;
}

static bool rule_applies(SiteCompatibilityRule const& rule, URL::Site const& top_level_site, URL::Site const& site)
{
    return rule.matches(top_level_site) || rule.matches(site);
}

String SiteCompatibilityData::user_agent_for(URL::Origin const& top_level_origin, URL::Origin const& origin, StringView default_user_agent) const
{
    auto top_level_site = URL::Site::obtain(top_level_origin);
    auto site = URL::Site::obtain(origin);

    auto user_agent = MUST(String::from_utf8(default_user_agent));
    for (auto const& rule : m_rules) {
        if (rule_applies(rule, top_level_site, site))
            user_agent = rule.apply_user_agent_transformations(user_agent);
    }
    return user_agent;
}

bool SiteCompatibilityData::exposes_experimental_interface(URL::Origin const& top_level_origin, URL::Origin const& origin, StringView name) const
{
    auto top_level_site = URL::Site::obtain(top_level_origin);
    auto site = URL::Site::obtain(origin);

    for (auto const& rule : m_rules) {
        if (rule.exposes_experimental_interface(name) && rule_applies(rule, top_level_site, site))
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
