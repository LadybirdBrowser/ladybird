/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/String.h>
#include <LibURL/Host.h>
#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>

#include <libpsl.h>

namespace URL {

static psl_ctx_t const* public_suffix_context()
{
    static auto const* context = psl_builtin();
    VERIFY(context);
    return context;
}

static constexpr auto public_suffix_match_types(PublicSuffixData::IncludeStarRule include_star_rule)
{
    if (include_star_rule == PublicSuffixData::IncludeStarRule::Yes)
        return PSL_TYPE_ANY;
    return PSL_TYPE_ANY | PSL_TYPE_NO_STAR_RULE;
}

struct NormalizedDomain {
    StringView host;
    StringView trailing_dot;
};

static Optional<NormalizedDomain> normalized_domain_for_host(Host const& host)
{
    if (!host.is_domain())
        return OptionalNone {};

    auto domain = host.get<String>().bytes_as_string_view().trim("."sv, TrimMode::Left);
    if (domain.is_empty())
        return OptionalNone {};

    auto trailing_dot = ""sv;
    if (domain.ends_with('.')) {
        trailing_dot = "."sv;
        domain = domain.substring_view(0, domain.length() - 1);
        if (domain.is_empty())
            return OptionalNone {};
    }

    return NormalizedDomain { domain, trailing_dot };
}

static bool is_matching_public_suffix_impl(StringView host, PublicSuffixData::IncludeStarRule include_star_rule)
{
    ByteString lookup_host { host };
    return psl_is_public_suffix2(public_suffix_context(), lookup_host.characters(), public_suffix_match_types(include_star_rule));
}

bool PublicSuffixData::is_matching_public_suffix(StringView host, IncludeStarRule include_star_rule)
{
    if (host.is_empty())
        return false;

    auto parsed_host = Parser::parse_host(host);
    if (!parsed_host.has_value())
        return false;

    return is_matching_public_suffix(*parsed_host, include_star_rule);
}

bool PublicSuffixData::is_matching_public_suffix(Host const& host, IncludeStarRule include_star_rule)
{
    auto normalized_domain = normalized_domain_for_host(host);
    if (!normalized_domain.has_value())
        return false;

    return is_matching_public_suffix_impl(normalized_domain->host, include_star_rule);
}

static Optional<String> find_matching_public_suffix_impl(StringView host, PublicSuffixData::IncludeStarRule include_star_rule)
{
    auto remaining_host = host;
    while (!remaining_host.is_empty()) {
        if (is_matching_public_suffix_impl(remaining_host, include_star_rule))
            return MUST(String::from_utf8(remaining_host));

        auto next_label_separator = remaining_host.find('.');
        if (!next_label_separator.has_value())
            return OptionalNone {};

        remaining_host = remaining_host.substring_view(*next_label_separator + 1);
    }

    return OptionalNone {};
}

Optional<String> PublicSuffixData::find_matching_public_suffix(StringView string, IncludeStarRule include_star_rule)
{
    if (string.is_empty())
        return {};

    auto parsed_host = Parser::parse_host(string);
    if (!parsed_host.has_value())
        return {};

    return find_matching_public_suffix(*parsed_host, include_star_rule);
}

Optional<String> PublicSuffixData::find_matching_public_suffix(Host const& host, IncludeStarRule include_star_rule)
{
    auto normalized_domain = normalized_domain_for_host(host);
    if (!normalized_domain.has_value())
        return {};

    auto public_suffix = find_matching_public_suffix_impl(normalized_domain->host, include_star_rule);
    if (!public_suffix.has_value())
        return {};

    return MUST(String::formatted("{}{}", public_suffix.value(), normalized_domain->trailing_dot));
}

static Optional<String> find_matching_registrable_domain_impl(StringView host, PublicSuffixData::IncludeStarRule include_star_rule)
{
    // find_matching_public_suffix_impl() always returns a tail of host, so it is by construction a suffix of it.
    auto public_suffix = find_matching_public_suffix_impl(host, include_star_rule);
    if (!public_suffix.has_value() || host == *public_suffix)
        return {};

    auto subhost = host.substring_view(0, host.length() - public_suffix->bytes_as_string_view().length());
    subhost = subhost.trim("."sv, TrimMode::Right);

    if (subhost.is_empty())
        return {};

    size_t start_index = 0;
    if (auto index = subhost.find_last('.'); index.has_value())
        start_index = *index + 1;

    return MUST(String::from_utf8(host.substring_view(start_index)));
}

Optional<String> PublicSuffixData::find_matching_registrable_domain(StringView string, IncludeStarRule include_star_rule)
{
    if (string.is_empty())
        return {};

    auto parsed_host = Parser::parse_host(string);
    if (!parsed_host.has_value())
        return {};

    return find_matching_registrable_domain(*parsed_host, include_star_rule);
}

Optional<String> PublicSuffixData::find_matching_registrable_domain(Host const& host, IncludeStarRule include_star_rule)
{
    auto normalized_domain = normalized_domain_for_host(host);
    if (!normalized_domain.has_value())
        return {};

    auto registrable_domain = find_matching_registrable_domain_impl(normalized_domain->host, include_star_rule);
    if (!registrable_domain.has_value())
        return {};

    return MUST(String::formatted("{}{}", registrable_domain.value(), normalized_domain->trailing_dot));
}

}
