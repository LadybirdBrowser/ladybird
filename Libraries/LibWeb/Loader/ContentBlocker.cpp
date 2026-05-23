/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibURL/Parser.h>
#include <LibWeb/ContentBlockerRustFFI.h>
#include <LibWeb/Loader/ContentBlocker.h>

namespace Web {

ContentBlocker& ContentBlocker::the()
{
    static ContentBlocker blocker;
    return blocker;
}

ContentBlocker::ContentBlocker() = default;

ContentBlocker::~ContentBlocker()
{
    ContentBlocking::FFI::rust_content_blocker_free(m_engine);
}

static StringView resource_type_to_adblock_request_type(ContentBlocker::ResourceType type)
{
    switch (type) {
    case ContentBlocker::ResourceType::Document:
        return "document"sv;
    case ContentBlocker::ResourceType::Font:
        return "font"sv;
    case ContentBlocker::ResourceType::Image:
        return "image"sv;
    case ContentBlocker::ResourceType::Media:
        return "media"sv;
    case ContentBlocker::ResourceType::Object:
        return "object"sv;
    case ContentBlocker::ResourceType::Other:
        return "other"sv;
    case ContentBlocker::ResourceType::Ping:
        return "ping"sv;
    case ContentBlocker::ResourceType::Script:
        return "script"sv;
    case ContentBlocker::ResourceType::Stylesheet:
        return "stylesheet"sv;
    case ContentBlocker::ResourceType::Subdocument:
        return "subdocument"sv;
    case ContentBlocker::ResourceType::WebSocket:
        return "websocket"sv;
    case ContentBlocker::ResourceType::XMLHttpRequest:
        return "xmlhttprequest"sv;
    }
    VERIFY_NOT_REACHED();
}

static ByteString serialized_url(URL::URL const& url)
{
    return url.serialize().to_byte_string();
}

static ByteString serialized_url_for_matching(URL::URL const& url)
{
    if (url.scheme() == "file"sv)
        return ByteString::formatted("http://local-file.invalid/{}", serialized_url(url));
    return serialized_url(url);
}

static String take_rust_string(ContentBlocking::FFI::ContentBlockerString rust_string)
{
    if (!rust_string.data)
        return {};

    ArmedScopeGuard free_string = [&] {
        ContentBlocking::FFI::rust_content_blocker_free_string(rust_string.data, rust_string.length);
    };

    auto maybe_string = String::from_utf8({ reinterpret_cast<char const*>(rust_string.data), rust_string.length });
    if (maybe_string.is_error())
        return {};
    return maybe_string.release_value();
}

static ErrorOr<String> join_lines(ReadonlySpan<String> lines)
{
    StringBuilder builder;
    for (auto const& line : lines) {
        builder.append(line);
        builder.append('\n');
    }
    return builder.to_string();
}

static bool line_looks_like_supported_cosmetic_rule(StringView line)
{
    auto trimmed_line = line.trim_whitespace();
    if (trimmed_line.is_empty() || trimmed_line.starts_with('!') || trimmed_line.starts_with('['))
        return false;

    auto sharp_index = trimmed_line.find('#');
    if (!sharp_index.has_value())
        return false;

    auto after_sharp_index = *sharp_index + 1;
    if (after_sharp_index >= trimmed_line.length())
        return false;

    auto second_sharp_index = trimmed_line.find('#', after_sharp_index);
    if (!second_sharp_index.has_value())
        return false;

    auto between_sharps = trimmed_line.substring_view(after_sharp_index, *second_sharp_index - after_sharp_index);
    if (between_sharps.starts_with('@')) {
        if (*sharp_index == 0)
            return false;
        between_sharps = between_sharps.substring_view(1);
    }
    if (between_sharps.starts_with('?'))
        between_sharps = between_sharps.substring_view(1);

    return between_sharps.is_empty();
}

static bool rules_contain_cosmetic_rules(ReadonlyBytes rules_bytes)
{
    bool has_cosmetic_rules = false;
    StringView { rules_bytes }.for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView line) {
        if (line_looks_like_supported_cosmetic_rule(line))
            has_cosmetic_rules = true;
    });
    return has_cosmetic_rules;
}

ErrorOr<void> ContentBlocker::set_patterns(ReadonlySpan<String> patterns)
{
    StringBuilder builder;
    for (auto const& pattern : patterns) {
        if (pattern.is_empty())
            continue;
        builder.append(pattern);
        builder.append('\n');
    }

    auto patterns_string = TRY(builder.to_string());
    auto patterns_bytes = patterns_string.bytes_as_string_view().bytes();
    return set_rules_from_bytes(patterns_bytes);
}

ErrorOr<void> ContentBlocker::set_rules_from_bytes(ReadonlyBytes rules_bytes)
{
    auto* engine = ContentBlocking::FFI::rust_content_blocker_create(
        rules_bytes.data(),
        rules_bytes.size());
    if (!engine)
        return Error::from_string_literal("Failed to create content blocker");

    auto has_cosmetic_rules = rules_contain_cosmetic_rules(rules_bytes);

    ContentBlocking::FFI::rust_content_blocker_free(m_engine);
    m_engine = engine;
    m_has_cosmetic_rules = has_cosmetic_rules;
    return {};
}

bool ContentBlocker::is_filtered(URL::URL const& url) const
{
    return is_filtered(url, url, ResourceType::Other);
}

bool ContentBlocker::is_filtered(URL::URL const& url, URL::URL const& source_url, Optional<Fetch::Infrastructure::Request::Destination> const& destination, Optional<Fetch::Infrastructure::Request::InitiatorType> const& initiator_type, Fetch::Infrastructure::Request::Mode mode) const
{
    return is_filtered(url, source_url_for_matching(source_url), resource_type_from_fetch_metadata(destination, initiator_type, mode));
}

bool ContentBlocker::is_filtered(URL::URL const& url, URL::URL const& source_url, ResourceType resource_type) const
{
    if (!filtering_enabled() || !m_engine)
        return false;

    if (url.scheme() == "data"sv)
        return false;

    auto url_string = serialized_url_for_matching(url);
    auto normalized_source_url = source_url_for_matching(source_url);
    auto source_url_string = serialized_url_for_matching(normalized_source_url);
    auto request_type = resource_type_to_adblock_request_type(resource_type);

    return ContentBlocking::FFI::rust_content_blocker_matches(
        m_engine,
        reinterpret_cast<u8 const*>(url_string.characters()),
        url_string.length(),
        reinterpret_cast<u8 const*>(source_url_string.characters()),
        source_url_string.length(),
        reinterpret_cast<u8 const*>(request_type.characters_without_null_termination()),
        request_type.length());
}

String ContentBlocker::cosmetic_style_sheet_for_url(URL::URL const& url) const
{
    return cosmetic_style_sheet_for_url(url, {}, {});
}

String ContentBlocker::cosmetic_style_sheet_for_url(URL::URL const& url, ReadonlySpan<String> classes, ReadonlySpan<String> ids) const
{
    if (!filtering_enabled() || !m_engine || !m_has_cosmetic_rules)
        return {};

    auto url_string = serialized_url(url);
    auto classes_string = join_lines(classes);
    if (classes_string.is_error())
        return {};

    auto ids_string = join_lines(ids);
    if (ids_string.is_error())
        return {};

    auto classes_bytes = classes_string.value().bytes_as_string_view();
    auto ids_bytes = ids_string.value().bytes_as_string_view();

    return take_rust_string(ContentBlocking::FFI::rust_content_blocker_cosmetic_css(
        m_engine,
        reinterpret_cast<u8 const*>(url_string.characters()),
        url_string.length(),
        reinterpret_cast<u8 const*>(classes_bytes.characters_without_null_termination()),
        classes_bytes.length(),
        reinterpret_cast<u8 const*>(ids_bytes.characters_without_null_termination()),
        ids_bytes.length()));
}

bool ContentBlocker::has_generic_cosmetic_selectors_for_url(URL::URL const& url, ReadonlySpan<String> classes, ReadonlySpan<String> ids) const
{
    if (!filtering_enabled() || !m_engine || !m_has_cosmetic_rules)
        return false;

    auto url_string = serialized_url(url);
    auto classes_string = join_lines(classes);
    if (classes_string.is_error())
        return false;

    auto ids_string = join_lines(ids);
    if (ids_string.is_error())
        return false;

    auto classes_bytes = classes_string.value().bytes_as_string_view();
    auto ids_bytes = ids_string.value().bytes_as_string_view();

    return ContentBlocking::FFI::rust_content_blocker_has_generic_cosmetic_selectors(
        m_engine,
        reinterpret_cast<u8 const*>(url_string.characters()),
        url_string.length(),
        reinterpret_cast<u8 const*>(classes_bytes.characters_without_null_termination()),
        classes_bytes.length(),
        reinterpret_cast<u8 const*>(ids_bytes.characters_without_null_termination()),
        ids_bytes.length());
}

ContentBlocker::ResourceType ContentBlocker::resource_type_from_fetch_metadata(Optional<Fetch::Infrastructure::Request::Destination> const& destination, Optional<Fetch::Infrastructure::Request::InitiatorType> const& initiator_type, Fetch::Infrastructure::Request::Mode mode)
{
    using Fetch::Infrastructure::Request;

    if (mode == Request::Mode::WebSocket)
        return ResourceType::WebSocket;

    if (destination.has_value()) {
        switch (*destination) {
        case Request::Destination::Audio:
        case Request::Destination::Track:
        case Request::Destination::Video:
            return ResourceType::Media;
        case Request::Destination::Document:
            return ResourceType::Document;
        case Request::Destination::Embed:
        case Request::Destination::Object:
            return ResourceType::Object;
        case Request::Destination::Font:
            return ResourceType::Font;
        case Request::Destination::Frame:
        case Request::Destination::IFrame:
            return ResourceType::Subdocument;
        case Request::Destination::Image:
            return ResourceType::Image;
        case Request::Destination::AudioWorklet:
        case Request::Destination::PaintWorklet:
        case Request::Destination::Script:
        case Request::Destination::ServiceWorker:
        case Request::Destination::SharedWorker:
        case Request::Destination::Worker:
            return ResourceType::Script;
        case Request::Destination::Style:
            return ResourceType::Stylesheet;
        case Request::Destination::JSON:
        case Request::Destination::Manifest:
        case Request::Destination::Report:
        case Request::Destination::WebIdentity:
        case Request::Destination::XSLT:
            return ResourceType::Other;
        }
        VERIFY_NOT_REACHED();
    }

    if (initiator_type.has_value()) {
        switch (*initiator_type) {
        case Request::InitiatorType::Audio:
        case Request::InitiatorType::Video:
        case Request::InitiatorType::Track:
            return ResourceType::Media;
        case Request::InitiatorType::Beacon:
        case Request::InitiatorType::Ping:
            return ResourceType::Ping;
        case Request::InitiatorType::Embed:
        case Request::InitiatorType::Object:
            return ResourceType::Object;
        case Request::InitiatorType::Fetch:
        case Request::InitiatorType::XMLHttpRequest:
            return ResourceType::XMLHttpRequest;
        case Request::InitiatorType::Font:
            return ResourceType::Font;
        case Request::InitiatorType::Frame:
        case Request::InitiatorType::IFrame:
            return ResourceType::Subdocument;
        case Request::InitiatorType::Image:
        case Request::InitiatorType::IMG:
            return ResourceType::Image;
        case Request::InitiatorType::Script:
            return ResourceType::Script;
        case Request::InitiatorType::CSS:
            return ResourceType::Stylesheet;
        case Request::InitiatorType::EarlyHint:
        case Request::InitiatorType::Body:
        case Request::InitiatorType::Input:
        case Request::InitiatorType::Link:
        case Request::InitiatorType::Other:
            return ResourceType::Other;
        }
        VERIFY_NOT_REACHED();
    }

    return ResourceType::Other;
}

URL::URL ContentBlocker::source_url_for_matching(URL::URL const& source_url)
{
    if (source_url.scheme() != "blob"sv)
        return source_url;

    auto parsed_url = URL::Parser::basic_parse(source_url.serialize_path());
    if (!parsed_url.has_value())
        return source_url;

    return parsed_url.release_value();
}

}
