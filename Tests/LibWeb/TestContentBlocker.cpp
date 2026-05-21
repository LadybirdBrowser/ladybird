/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Loader/ContentBlocker.h>

namespace Web {

static ContentBlocker& make_blocker(Vector<String> patterns)
{
    auto& blocker = ContentBlocker::the();
    MUST(blocker.set_patterns(patterns));
    return blocker;
}

static URL::URL url(StringView string)
{
    auto result = URL::Parser::basic_parse(string);
    EXPECT(result.has_value());
    return result.release_value();
}

TEST_CASE(empty_pattern_list)
{
    auto& blocker = make_blocker({});

    EXPECT(!blocker.is_filtered(url("https://anything.com"sv)));
    EXPECT(!blocker.is_filtered(url("data:text/plain,hi"sv)));
}

TEST_CASE(basic_blocking)
{
    Vector<String> patterns = {
        "ads."_string,
        "?banner"_string,
        "tracker"_string
    };

    auto& blocker = make_blocker(move(patterns));

    EXPECT(blocker.is_filtered(url("https://example.com/ads.js"sv)));
    EXPECT(blocker.is_filtered(url("http://site.com/page.html?banner=true"sv)));
    EXPECT(blocker.is_filtered(url("https://tracker.example.org/ping"sv)));
    EXPECT(!blocker.is_filtered(url("https://ds.example.com/page.html"sv)));
}

TEST_CASE(data_urls_exempt)
{
    Vector<String> patterns = {
        { "data:"_string },
        { "evil.com"_string }
    };

    auto& blocker = make_blocker(move(patterns));

    EXPECT(!blocker.is_filtered(url("data:text/plain,hello"sv)));
    EXPECT(!blocker.is_filtered(url("data:image/png;base64,abc123"sv)));
    EXPECT(blocker.is_filtered(url("https://evil.com/script.js"sv)));
}

TEST_CASE(disable_filtering)
{
    Vector<String> patterns = {
        { "example.com"_string },
        { "##.ad"_string }
    };

    auto& blocker = make_blocker(move(patterns));
    blocker.set_filtering_enabled(false);

    EXPECT(!blocker.is_filtered(url("https://example.com"sv)));
    EXPECT(!blocker.is_filtered(url("http://example.com/ads"sv)));
    EXPECT(blocker.cosmetic_style_sheet_for_url(url("https://example.com"sv)).is_empty());

    blocker.set_filtering_enabled(true);
    EXPECT(blocker.is_filtered(url("https://example.com"sv)));
    EXPECT(!blocker.cosmetic_style_sheet_for_url(url("https://example.com"sv)).is_empty());
}

TEST_CASE(substring_matches)
{
    Vector<String> patterns = {
        { "ads"_string },
        { "ad/"_string }
    };

    auto& blocker = make_blocker(move(patterns));

    EXPECT(blocker.is_filtered(url("https://site.com/ads/banner.jpg"sv)));
    EXPECT(blocker.is_filtered(url("http://marketing.com/ad/page"sv)));
    EXPECT(!blocker.is_filtered(url("https://site.com/content/article.html"sv)));
    EXPECT(!blocker.is_filtered(url("http://advancedtech.com/home"sv)));
}

TEST_CASE(file_scheme_can_be_filtered)
{
    Vector<String> patterns = {
        { "secret"_string },
        { ".txt"_string }
    };

    auto& blocker = make_blocker(move(patterns));

    EXPECT(blocker.is_filtered(url("file:///home/user/secret.txt"sv)));
    EXPECT(!blocker.is_filtered(url("file:///home/user/document.pdf"sv)));
}

TEST_CASE(query_parameters_and_fragments)
{
    Vector<String> patterns = {
        { "#ad="_string },
        { "?ad="_string },
        { "#sponsored"_string }
    };

    auto& blocker = make_blocker(move(patterns));

    EXPECT(blocker.is_filtered(url("https://site.com/page?ad=123"sv)));
    EXPECT(blocker.is_filtered(url("https://site.com/page#ad=456"sv)));
    EXPECT(blocker.is_filtered(url("https://site.com/page?ref=home&ad=1#sponsored"sv)));
    EXPECT(!blocker.is_filtered(url("https://site.com/page?ref=home"sv)));
}

TEST_CASE(fetch_metadata_maps_to_resource_type)
{
    using Request = Fetch::Infrastructure::Request;
    using ResourceType = ContentBlocker::ResourceType;

    auto resource_type = [](Optional<Request::Destination> destination, Optional<Request::InitiatorType> initiator_type, Request::Mode mode = Request::Mode::NoCORS) {
        return ContentBlocker::resource_type_from_fetch_metadata(destination, initiator_type, mode);
    };

    EXPECT(resource_type(Request::Destination::Document, {}) == ResourceType::Document);
    EXPECT(resource_type(Request::Destination::Font, Request::InitiatorType::CSS) == ResourceType::Font);
    EXPECT(resource_type(Request::Destination::Image, Request::InitiatorType::CSS) == ResourceType::Image);
    EXPECT(resource_type(Request::Destination::Style, {}) == ResourceType::Stylesheet);
    EXPECT(resource_type(Request::Destination::IFrame, {}) == ResourceType::Subdocument);
    EXPECT(resource_type({}, Request::InitiatorType::Fetch) == ResourceType::XMLHttpRequest);
    EXPECT(resource_type({}, Request::InitiatorType::Ping) == ResourceType::Ping);
    EXPECT(resource_type({}, {}, Request::Mode::WebSocket) == ResourceType::WebSocket);
}

TEST_CASE(blob_source_urls_are_normalized_for_matching)
{
    auto normalized = ContentBlocker::source_url_for_matching(url("blob:https://example.com/object-id"sv));
    EXPECT_EQ(normalized.to_string(), "https://example.com/object-id"sv);

    auto non_blob = ContentBlocker::source_url_for_matching(url("https://example.com/page"sv));
    EXPECT_EQ(non_blob.to_string(), "https://example.com/page"sv);
}

TEST_CASE(contextual_filtering_uses_existing_matcher)
{
    Vector<String> patterns = {
        { "blocked.js"_string }
    };

    auto& blocker = make_blocker(move(patterns));

    EXPECT(blocker.is_filtered(
        url("https://tracker.example/blocked.js"sv),
        url("https://example.com/page"sv),
        Fetch::Infrastructure::Request::Destination::Script,
        Fetch::Infrastructure::Request::InitiatorType::CSS,
        Fetch::Infrastructure::Request::Mode::NoCORS));
}

TEST_CASE(cosmetic_rules_generate_user_css)
{
    Vector<String> patterns = {
        { "blocked.js"_string },
        { "##.ad"_string },
        { "example.com##.sponsored"_string },
        { "other.example##.other"_string }
    };

    auto& blocker = make_blocker(move(patterns));

    EXPECT(blocker.has_cosmetic_rules());
    EXPECT(blocker.is_filtered(url("https://tracker.example/blocked.js"sv)));
    EXPECT(!blocker.is_filtered(url("https://tracker.example/##.ad"sv)));

    auto style_sheet = blocker.cosmetic_style_sheet_for_url(url("https://www.example.com/page"sv));
    EXPECT(style_sheet.contains(".ad { display: none !important; }"sv));
    EXPECT(style_sheet.contains(".sponsored { display: none !important; }"sv));
    EXPECT(!style_sheet.contains(".other { display: none !important; }"sv));
}

TEST_CASE(clearing_patterns_clears_cosmetic_rules)
{
    auto& blocker = make_blocker({ "##.ad"_string });
    EXPECT(blocker.has_cosmetic_rules());

    MUST(blocker.set_patterns({}));
    EXPECT(!blocker.has_cosmetic_rules());
    EXPECT(blocker.cosmetic_style_sheet_for_url(url("https://example.com/"sv)).is_empty());
}

}
