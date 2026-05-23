/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Loader/ContentBlocker.h>

namespace Web {

static ContentBlocker& make_blocker(Vector<String> rules)
{
    auto& blocker = ContentBlocker::the();
    MUST(blocker.set_patterns(rules));
    blocker.set_filtering_enabled(true);
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
    Vector<String> rules = {
        "||ads.example.com^"_string,
        "/banner.js"_string,
    };

    auto& blocker = make_blocker(move(rules));
    auto source_url = url("https://example.com/"sv);

    EXPECT(blocker.is_filtered(url("https://ads.example.com/script.js"sv), source_url, ContentBlocker::ResourceType::Script));
    EXPECT(blocker.is_filtered(url("https://static.example.com/banner.js"sv), source_url, ContentBlocker::ResourceType::Script));
    EXPECT(!blocker.is_filtered(url("https://example.com/page.html"sv), source_url, ContentBlocker::ResourceType::Document));
}

TEST_CASE(data_urls_exempt)
{
    Vector<String> rules = {
        { "data:"_string },
        { "||evil.com^"_string }
    };

    auto& blocker = make_blocker(move(rules));
    auto source_url = url("https://example.com/"sv);

    EXPECT(!blocker.is_filtered(url("data:text/plain,hello"sv)));
    EXPECT(!blocker.is_filtered(url("data:image/png;base64,abc123"sv)));
    EXPECT(blocker.is_filtered(url("https://evil.com/script.js"sv), source_url, ContentBlocker::ResourceType::Script));
}

TEST_CASE(invalid_filter_bytes_keep_previous_rules)
{
    Vector<String> rules = {
        { "||ads.example.com^"_string },
    };

    auto& blocker = make_blocker(move(rules));
    auto source_url = url("https://example.com/"sv);

    EXPECT(blocker.is_filtered(url("https://ads.example.com/script.js"sv), source_url, ContentBlocker::ResourceType::Script));

    Array<u8, 1> invalid_utf8 { 0xff };
    auto result = blocker.set_rules_from_bytes(invalid_utf8.span());
    EXPECT(result.is_error());

    EXPECT(blocker.is_filtered(url("https://ads.example.com/script.js"sv), source_url, ContentBlocker::ResourceType::Script));
}

TEST_CASE(disable_filtering)
{
    Vector<String> rules = {
        { "||example.com^"_string },
        { "##.ad"_string }
    };

    auto& blocker = make_blocker(move(rules));
    blocker.set_filtering_enabled(false);
    Vector<String> classes = { "ad"_string };
    Vector<String> ids;

    EXPECT(!blocker.is_filtered(url("https://example.com"sv)));
    EXPECT(!blocker.is_filtered(url("http://example.com/ads"sv)));
    EXPECT(blocker.cosmetic_style_sheet_for_url(url("https://example.com"sv), classes.span(), ids.span()).is_empty());

    blocker.set_filtering_enabled(true);
    EXPECT(blocker.is_filtered(url("https://example.com"sv)));
    EXPECT(!blocker.cosmetic_style_sheet_for_url(url("https://example.com"sv), classes.span(), ids.span()).is_empty());
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

TEST_CASE(resource_type_options)
{
    using Request = Fetch::Infrastructure::Request;

    Vector<String> rules = {
        { "||example.com/ad^$image"_string },
        { "||example.com/font^$font"_string },
        { "||example.com/document^$document"_string },
        { "||example.com/fetch^$xmlhttprequest"_string },
    };

    auto& blocker = make_blocker(move(rules));
    auto source_url = url("https://example.com/"sv);

    EXPECT(blocker.is_filtered(url("https://example.com/ad"sv), source_url, ContentBlocker::ResourceType::Image));
    EXPECT(!blocker.is_filtered(url("https://example.com/ad"sv), source_url, ContentBlocker::ResourceType::Script));
    EXPECT(blocker.is_filtered(
        url("https://example.com/ad"sv),
        source_url,
        Request::Destination::Image,
        Request::InitiatorType::CSS,
        Request::Mode::NoCORS));
    EXPECT(blocker.is_filtered(
        url("https://example.com/font"sv),
        source_url,
        Request::Destination::Font,
        Request::InitiatorType::CSS,
        Request::Mode::NoCORS));
    EXPECT(blocker.is_filtered(
        url("https://example.com/document"sv),
        source_url,
        Request::Destination::Document,
        Optional<Request::InitiatorType> {},
        Request::Mode::Navigate));
    EXPECT(blocker.is_filtered(
        url("https://example.com/fetch"sv),
        source_url,
        Optional<Request::Destination> {},
        Request::InitiatorType::Fetch,
        Request::Mode::CORS));
}

TEST_CASE(file_scheme_fallback)
{
    Vector<String> rules = {
        { "secret"_string },
    };

    auto& blocker = make_blocker(move(rules));

    EXPECT(blocker.is_filtered(url("file:///home/user/secret.txt"sv)));
    EXPECT(!blocker.is_filtered(url("file:///home/user/public.txt"sv)));
}

TEST_CASE(third_party_option)
{
    Vector<String> rules = {
        { "||tracker.example^$third-party"_string },
    };

    auto& blocker = make_blocker(move(rules));

    EXPECT(blocker.is_filtered(url("https://tracker.example/pixel.gif"sv), url("https://site.example/"sv), ContentBlocker::ResourceType::Image));
    EXPECT(!blocker.is_filtered(url("https://tracker.example/pixel.gif"sv), url("https://www.tracker.example/"sv), ContentBlocker::ResourceType::Image));
}

TEST_CASE(blob_source_url_uses_embedded_url)
{
    Vector<String> rules = {
        { "||tracker.example/third-party.gif$third-party"_string },
        { "||tracker.example/first-party.gif$first-party"_string },
        { "||ads.example/domain.js$domain=www.tracker.example"_string },
    };

    auto& blocker = make_blocker(move(rules));
    auto same_site_blob_source = url("blob:https://www.tracker.example/4dd3d7ea-8bd7-4fe0-a121-79c18e2be4b2"sv);
    auto cross_site_blob_source = url("blob:https://site.example/4dd3d7ea-8bd7-4fe0-a121-79c18e2be4b2"sv);

    EXPECT(!blocker.is_filtered(url("https://tracker.example/third-party.gif"sv), same_site_blob_source, ContentBlocker::ResourceType::Image));
    EXPECT(blocker.is_filtered(url("https://tracker.example/first-party.gif"sv), same_site_blob_source, ContentBlocker::ResourceType::Image));
    EXPECT(blocker.is_filtered(url("https://tracker.example/third-party.gif"sv), cross_site_blob_source, ContentBlocker::ResourceType::Image));
    EXPECT(!blocker.is_filtered(url("https://tracker.example/first-party.gif"sv), cross_site_blob_source, ContentBlocker::ResourceType::Image));
    EXPECT(blocker.is_filtered(url("https://ads.example/domain.js"sv), same_site_blob_source, ContentBlocker::ResourceType::Script));
    EXPECT(!blocker.is_filtered(url("https://ads.example/domain.js"sv), cross_site_blob_source, ContentBlocker::ResourceType::Script));
}

TEST_CASE(document_navigation_is_first_party_to_itself)
{
    Vector<String> rules = {
        { "||localhost^$third-party,document"_string },
    };

    auto& blocker = make_blocker(move(rules));
    auto target_url = url("http://localhost:1234/content-blocker-target"sv);

    EXPECT(blocker.is_filtered(target_url, url("https://source.example/"sv), ContentBlocker::ResourceType::Document));
    EXPECT(!blocker.is_filtered(target_url, target_url, ContentBlocker::ResourceType::Document));
}

TEST_CASE(exception_rules)
{
    Vector<String> rules = {
        { "||ads.example.com^"_string },
        { "@@||ads.example.com/allowed.js"_string },
    };

    auto& blocker = make_blocker(move(rules));
    auto source_url = url("https://example.com/"sv);

    EXPECT(blocker.is_filtered(url("https://ads.example.com/blocked.js"sv), source_url, ContentBlocker::ResourceType::Script));
    EXPECT(!blocker.is_filtered(url("https://ads.example.com/allowed.js"sv), source_url, ContentBlocker::ResourceType::Script));
}

TEST_CASE(cosmetic_style_sheet)
{
    Vector<String> rules = {
        { "example.com##.ad-banner"_string },
        { "example.com###sponsor"_string },
        { "example.com##.styled-sponsor:style(visibility: hidden)"_string },
        { "##.generic-ad"_string },
        { "###generic-sponsor"_string },
    };

    auto& blocker = make_blocker(move(rules));
    Vector<String> classes = { "generic-ad"_string };
    Vector<String> ids = { "generic-sponsor"_string };

    auto style_sheet = blocker.cosmetic_style_sheet_for_url(url("https://example.com/"sv), classes, ids);

    EXPECT(style_sheet.contains(".ad-banner { display: none !important; }"sv));
    EXPECT(style_sheet.contains("#sponsor { display: none !important; }"sv));
    EXPECT(style_sheet.contains(".styled-sponsor { visibility: hidden; }"sv));
    EXPECT(style_sheet.contains(".generic-ad { display: none !important; }"sv));
    EXPECT(style_sheet.contains("#generic-sponsor { display: none !important; }"sv));
}

TEST_CASE(generic_cosmetic_selector_lists_match_later_selectors)
{
    Vector<String> rules = {
        { "##.first-ad-class, .second-ad-class"_string },
        { "##.first-ad-id, #second-ad-id"_string },
        { "##.class-keyed-arm, [data-ad]"_string },
        { "##.class-keyed-arm, div.sponsor"_string },
    };

    auto& blocker = make_blocker(move(rules));
    Vector<String> classes = { "second-ad-class"_string };
    Vector<String> ids = { "second-ad-id"_string };
    Vector<String> no_classes;
    Vector<String> no_ids;

    auto style_sheet = blocker.cosmetic_style_sheet_for_url(url("https://example.com/"sv), classes, ids);
    auto style_sheet_without_class_or_id_hints = blocker.cosmetic_style_sheet_for_url(url("https://example.com/"sv), no_classes, no_ids);

    EXPECT(style_sheet.contains(".first-ad-class, .second-ad-class { display: none !important; }"sv));
    EXPECT(style_sheet.contains(".first-ad-id, #second-ad-id { display: none !important; }"sv));
    EXPECT(style_sheet_without_class_or_id_hints.contains(".class-keyed-arm, [data-ad] { display: none !important; }"sv));
    EXPECT(style_sheet_without_class_or_id_hints.contains(".class-keyed-arm, div.sponsor { display: none !important; }"sv));
}

TEST_CASE(cosmetic_rule_detection)
{
    auto& blocker_without_cosmetics = make_blocker({ "||example.com/ad^"_string });
    EXPECT(!blocker_without_cosmetics.has_cosmetic_rules());
    EXPECT(blocker_without_cosmetics.cosmetic_style_sheet_for_url(url("https://example.com/"sv), {}, {}).is_empty());

    auto& blocker_with_cosmetics = make_blocker({ "example.com##.ad-banner"_string });
    EXPECT(blocker_with_cosmetics.has_cosmetic_rules());
}

}
