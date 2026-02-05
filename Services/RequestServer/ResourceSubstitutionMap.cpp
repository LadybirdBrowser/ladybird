/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/File.h>
#include <LibURL/Parser.h>
#include <RequestServer/ResourceSubstitutionMap.h>

namespace RequestServer {

static String normalize_url(URL::URL const& url)
{
    auto normalized = url;
    normalized.set_query({});
    normalized.set_fragment({});
    return normalized.serialize();
}

ErrorOr<NonnullOwnPtr<ResourceSubstitutionMap>> ResourceSubstitutionMap::load_from_file(StringView path)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto content = TRY(file->read_until_eof());
    auto json = TRY(JsonValue::from_string(content));

    if (!json.is_object())
        return Error::from_string_literal("Resource substitution map must be a JSON object");

    auto const& root = json.as_object();
    auto substitutions_value = root.get("substitutions"sv);

    if (!substitutions_value.has_value() || !substitutions_value->is_array())
        return Error::from_string_literal("Resource substitution map must contain a 'substitutions' array");

    auto map = adopt_own(*new ResourceSubstitutionMap);

    for (auto const& entry : substitutions_value->as_array().values()) {
        if (!entry.is_object()) {
            warnln("Skipping non-object entry in resource substitution map");
            continue;
        }

        auto const& obj = entry.as_object();

        auto url_value = obj.get("url"sv);
        auto file_value = obj.get("file"sv);

        if (!url_value.has_value() || !url_value->is_string()) {
            warnln("Skipping entry without valid 'url' string");
            continue;
        }

        if (!file_value.has_value() || !file_value->is_string()) {
            warnln("Skipping entry without valid 'file' string");
            continue;
        }

        ResourceSubstitution substitution;
        substitution.file_path = file_value->as_string().to_byte_string();

        if (auto content_type_value = obj.get("content_type"sv); content_type_value.has_value() && content_type_value->is_string())
            substitution.content_type = content_type_value->as_string();

        if (auto status_code_value = obj.get("status_code"sv); status_code_value.has_value() && status_code_value->is_integer<u32>())
            substitution.status_code = status_code_value->as_integer<u32>();

        auto url = URL::Parser::basic_parse(url_value->as_string());
        if (!url.has_value()) {
            warnln("Skipping entry with invalid URL '{}'", url_value->as_string());
            continue;
        }

        map->m_substitutions.set(normalize_url(*url), move(substitution));
    }

    return map;
}

Optional<ResourceSubstitution const&> ResourceSubstitutionMap::lookup(URL::URL const& url) const
{
    auto it = m_substitutions.find(normalize_url(url));
    if (it == m_substitutions.end())
        return {};
    return it->value;
}

}
