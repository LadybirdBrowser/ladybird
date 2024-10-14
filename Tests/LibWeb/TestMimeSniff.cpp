/*
 * Copyright (c) 2023-2024, Kemal Zebari <kemalzebra@gmail.com>.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/MimeSniff/MimeType.h>

#include <LibWeb/MimeSniff/Resource.h>

TEST_CASE(determine_computed_mime_type_given_no_sniff_is_set)
{
    auto mime_type = Web::MimeSniff::MimeType::create("text"_string, "html"_string);
    auto computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"sv.bytes(), Web::MimeSniff::SniffingConfiguration { .supplied_type = mime_type, .no_sniff = true });

    EXPECT_EQ("text/html"sv, computed_mime_type.serialized());

    // Cover the edge case in the context-specific sniffing algorithm.
    computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                               .sniffing_context = Web::MimeSniff::SniffingContext::Image,
                                                                               .supplied_type = mime_type,
                                                                               .no_sniff = true,
                                                                           });

    EXPECT_EQ("text/html"sv, computed_mime_type.serialized());
}

TEST_CASE(determine_computed_mime_type_given_no_sniff_is_unset)
{
    auto supplied_type = Web::MimeSniff::MimeType::create("application"_string, "x-this-is-a-test"_string);
    auto computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"sv.bytes(), Web::MimeSniff::SniffingConfiguration { .supplied_type = supplied_type });

    EXPECT_EQ("application/x-this-is-a-test"sv, computed_mime_type.serialized());
}

TEST_CASE(determine_computed_mime_type_given_xml_mime_type_as_supplied_type)
{
    auto xml_mime_type = "application/rss+xml"sv;
    auto supplied_type = Web::MimeSniff::MimeType::parse(xml_mime_type).release_value();
    auto computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"sv.bytes(), Web::MimeSniff::SniffingConfiguration { .supplied_type = supplied_type });

    EXPECT_EQ(xml_mime_type, computed_mime_type.serialized());
}

static void set_image_type_mappings(HashMap<StringView, Vector<StringView>>& mime_type_to_headers_map)
{
    mime_type_to_headers_map.set("image/x-icon"sv, { "\x00\x00\x01\x00"sv, "\x00\x00\x02\x00"sv });
    mime_type_to_headers_map.set("image/bmp"sv, { "BM"sv });
    mime_type_to_headers_map.set("image/gif"sv, { "GIF87a"sv, "GIF89a"sv });
    mime_type_to_headers_map.set("image/webp"sv, { "RIFF\x00\x00\x00\x00WEBPVP"sv });
    mime_type_to_headers_map.set("image/png"sv, { "\x89PNG\x0D\x0A\x1A\x0A"sv });
    mime_type_to_headers_map.set("image/jpeg"sv, { "\xFF\xD8\xFF"sv });
}

static void set_audio_or_video_type_mappings(HashMap<StringView, Vector<StringView>>& mime_type_to_headers_map)
{
    mime_type_to_headers_map.set("audio/aiff"sv, { "FORM\x00\x00\x00\x00\x41IFF"sv });
    mime_type_to_headers_map.set("audio/mpeg"sv, { "ID3"sv });
    mime_type_to_headers_map.set("application/ogg"sv, { "OggS\x00"sv });
    mime_type_to_headers_map.set("audio/midi"sv, { "MThd\x00\x00\x00\x06"sv });
    mime_type_to_headers_map.set("video/avi"sv, { "RIFF\x00\x00\x00\x00\x41\x56\x49\x20"sv });
    mime_type_to_headers_map.set("audio/wave"sv, { "RIFF\x00\x00\x00\x00WAVE"sv });
}

static void set_text_plain_type_mappings(HashMap<StringView, Vector<StringView>>& mime_type_to_headers_map)
{
    mime_type_to_headers_map.set("text/plain"sv, {
                                                     "\xFE\xFF\x00\x00"sv,
                                                     "\xFF\xFE\x00\x00"sv,
                                                     "\xEF\xBB\xBF\x00"sv,
                                                     "Hello world!"sv,
                                                 });
}

TEST_CASE(determine_computed_mime_type_given_supplied_type_that_is_an_apache_bug_mime_type)
{
    Vector<StringView> apache_bug_mime_types = {
        "text/plain"sv,
        "text/plain; charset=ISO-8859-1"sv,
        "text/plain; charset=iso-8859-1"sv,
        "text/plain; charset=UTF-8"sv
    };

    // Cover all Apache bug MIME types.
    for (auto const& apache_bug_mime_type : apache_bug_mime_types) {
        auto supplied_type = Web::MimeSniff::MimeType::parse(apache_bug_mime_type).release_value();
        auto computed_mime_type = Web::MimeSniff::Resource::sniff("Hello world!"sv.bytes(),
            Web::MimeSniff::SniffingConfiguration { .scheme = "http"sv, .supplied_type = supplied_type });

        EXPECT_EQ("text/plain"sv, computed_mime_type.serialized());
    }

    // Cover all code paths in "rules for distinguishing if a resource is text or binary".
    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;
    mime_type_to_headers_map.set("application/octet-stream"sv, { "\x00"sv });

    set_text_plain_type_mappings(mime_type_to_headers_map);

    auto supplied_type = Web::MimeSniff::MimeType::create("text"_string, "plain"_string);
    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(),
                Web::MimeSniff::SniffingConfiguration { .scheme = "http"sv, .supplied_type = supplied_type });

            EXPECT_EQ(mime_type, computed_mime_type.serialized());
        }
    }
}

TEST_CASE(determine_computed_mime_type_given_xml_or_html_supplied_type)
{
    // With HTML supplied type.
    auto config = Web::MimeSniff::SniffingConfiguration { .supplied_type = Web::MimeSniff::MimeType::create("text"_string, "html"_string) };
    auto computed_mime_type = Web::MimeSniff::Resource::sniff(""sv.bytes(), config);
    EXPECT_EQ("text/html"sv, computed_mime_type.serialized());

    // With XML supplied type.
    config = Web::MimeSniff::SniffingConfiguration { .supplied_type = Web::MimeSniff::MimeType::create("text"_string, "xml"_string) };
    computed_mime_type = Web::MimeSniff::Resource::sniff(""sv.bytes(), config);
    EXPECT_EQ("text/xml"sv, computed_mime_type.serialized());
}

TEST_CASE(determine_computed_mime_type_in_both_none_and_browsing_sniffing_context)
{
    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    mime_type_to_headers_map.set("application/octet-stream"sv, { "\x00"sv });
    mime_type_to_headers_map.set("text/html"sv, {
                                                    "\x09\x09<!DOCTYPE HTML\x20"sv,
                                                    "\x0A<HTML\x3E"sv,
                                                    "\x0C<HEAD\x20"sv,
                                                    "\x0D<SCRIPT>"sv,
                                                    "\x20<IFRAME>"sv,
                                                    "<H1>"sv,
                                                    "<DIV>"sv,
                                                    "<FONT>"sv,
                                                    "<TABLE>"sv,
                                                    "<A>"sv,
                                                    "<STYLE>"sv,
                                                    "<TITLE>"sv,
                                                    "<B>"sv,
                                                    "<BODY>"sv,
                                                    "<BR>"sv,
                                                    "<P>"sv,
                                                    "<!-->"sv,
                                                });
    mime_type_to_headers_map.set("text/xml"sv, { "<?xml"sv });
    mime_type_to_headers_map.set("application/pdf"sv, { "%PDF-"sv });
    mime_type_to_headers_map.set("application/postscript"sv, { "%!PS-Adobe-"sv });

    set_text_plain_type_mappings(mime_type_to_headers_map);
    set_image_type_mappings(mime_type_to_headers_map);
    set_audio_or_video_type_mappings(mime_type_to_headers_map);

    mime_type_to_headers_map.set("application/x-gzip"sv, { "\x1F\x8B\x08"sv });
    mime_type_to_headers_map.set("application/zip"sv, { "PK\x03\x04"sv });
    mime_type_to_headers_map.set("application/x-rar-compressed"sv, { "Rar\x20\x1A\x07\x00"sv });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {

            // Test in a non-specific sniffing context.
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes());
            EXPECT_EQ(mime_type, computed_mime_type.essence());

            // Test sniffing in a browsing context.
            computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Browsing });
            EXPECT_EQ(mime_type, computed_mime_type.essence());
        }
    }
}

TEST_CASE(compute_mime_type_given_unknown_supplied_type)
{
    Array<Web::MimeSniff::MimeType, 3> unknown_supplied_types = {
        Web::MimeSniff::MimeType::create("unknown"_string, "unknown"_string),
        Web::MimeSniff::MimeType::create("application"_string, "unknown"_string),
        Web::MimeSniff::MimeType::create("*"_string, "*"_string)
    };
    auto header_bytes = "<HTML>"sv.bytes();

    for (auto const& unknown_supplied_type : unknown_supplied_types) {
        auto computed_mime_type = Web::MimeSniff::Resource::sniff(header_bytes, Web::MimeSniff::SniffingConfiguration { .supplied_type = unknown_supplied_type });
        EXPECT_EQ("text/html"sv, computed_mime_type.essence());
    }
}

TEST_CASE(determine_computed_mime_type_in_image_sniffing_context)
{
    // Cover case where supplied type is an XML MIME type.
    auto mime_type = "application/rss+xml"sv;
    auto supplied_type = Web::MimeSniff::MimeType::parse(mime_type).release_value();
    auto computed_mime_type = Web::MimeSniff::Resource::sniff(""sv.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Image, .supplied_type = supplied_type });

    EXPECT_EQ(mime_type, computed_mime_type.serialized());

    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    set_image_type_mappings(mime_type_to_headers_map);

    // Also consider a resource that is not an image.
    mime_type_to_headers_map.set("application/octet-stream"sv, { "\x00"sv });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Image });
            EXPECT_EQ(mime_type, computed_mime_type.essence());
        }
    }

    // Cover case where we aren't dealing with an image MIME type.
    mime_type = "text/html"sv;
    supplied_type = Web::MimeSniff::MimeType::parse("text/html"sv).release_value();
    computed_mime_type = Web::MimeSniff::Resource::sniff(""sv.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Image, .supplied_type = supplied_type });

    EXPECT_EQ(mime_type, computed_mime_type.essence());
}

TEST_CASE(determine_computed_mime_type_in_audio_or_video_sniffing_context)
{
    // Cover case where supplied type is an XML MIME type.
    auto mime_type = "application/rss+xml"sv;
    auto supplied_type = Web::MimeSniff::MimeType::parse(mime_type).release_value();
    auto computed_mime_type = Web::MimeSniff::Resource::sniff(""sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                                .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo,
                                                                                .supplied_type = supplied_type,
                                                                            });

    EXPECT_EQ(mime_type, computed_mime_type.serialized());
    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    set_audio_or_video_type_mappings(mime_type_to_headers_map);

    // Also consider a resource that is not an audio or video.
    mime_type_to_headers_map.set("application/octet-stream"sv, { "\x00"sv });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo });
            EXPECT_EQ(mime_type, computed_mime_type.essence());
        }
    }

    // Cover case where we aren't dealing with an audio or video MIME type.
    mime_type = "text/html"sv;
    supplied_type = Web::MimeSniff::MimeType::parse("text/html"sv).release_value();
    computed_mime_type = Web::MimeSniff::Resource::sniff(""sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                           .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo,
                                                                           .supplied_type = supplied_type,
                                                                       });

    EXPECT_EQ(mime_type, computed_mime_type.essence());
}

TEST_CASE(determine_computed_mime_type_when_trying_to_match_mp4_signature)
{
    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    mime_type_to_headers_map.set("application/octet-stream"sv, {
                                                                   // Payload length < 12.
                                                                   "!= 12"sv,
                                                                   // Payload length < box size.
                                                                   "\x00\x00\x00\x1F\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A"sv,
                                                                   // Box size % 4 != 0.
                                                                   "\x00\x00\x00\x0D\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"sv,
                                                                   // 4 bytes after box size header != "ftyp".
                                                                   "\x00\x00\x00\x0C\x00\x00\x00\x00\x00\x00\x00\x00"sv,
                                                                   // Sequence "mp4" couldn't be found in ftyp box.
                                                                   "\x00\x00\x00\x18\x66\x74\x79\x70isom\x00\x00\x00\x00\x61\x76\x63\x31\x00\x00\x00\x00"sv,
                                                               });
    mime_type_to_headers_map.set("video/mp4"sv, {
                                                    // 3 bytes after "ftyp" sequence == "mp4".
                                                    "\x00\x00\x00\x0C\x66\x74\x79\x70mp42"sv,
                                                    // "mp4" sequence found while executing while loop (this input covers entire loop)
                                                    "\x00\x00\x00\x18\x66\x74\x79\x70isom\x00\x00\x00\x00\x61\x76\x63\x31mp41"sv,
                                                });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo });
            EXPECT_EQ(mime_type, computed_mime_type.serialized());
        }
    }
}

TEST_CASE(determine_computed_mime_type_in_a_font_context)
{
    // Cover case where supplied type is an XML MIME type.
    auto mime_type = "application/rss+xml"sv;
    auto supplied_type = Web::MimeSniff::MimeType::parse(mime_type).release_value();
    auto computed_mime_type = Web::MimeSniff::Resource::sniff(""sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                                .sniffing_context = Web::MimeSniff::SniffingContext::Font,
                                                                                .supplied_type = supplied_type,
                                                                            });

    EXPECT_EQ(mime_type, computed_mime_type.serialized());

    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;
    mime_type_to_headers_map.set("application/octet-stream"sv, { "\x00"sv });
    mime_type_to_headers_map.set("application/vnd.ms-fontobject"sv, { "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00LP"sv });
    mime_type_to_headers_map.set("font/ttf"sv, { "\x00\x01\x00\x00"sv });
    mime_type_to_headers_map.set("font/otf"sv, { "OTTO"sv });
    mime_type_to_headers_map.set("font/collection"sv, { "ttcf"sv });
    mime_type_to_headers_map.set("font/woff"sv, { "wOFF"sv });
    mime_type_to_headers_map.set("font/woff2"sv, { "wOF2"sv });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Font });
            EXPECT_EQ(mime_type, computed_mime_type.essence());
        }
    }

    // Cover case where we aren't dealing with a font MIME type.
    mime_type = "text/html"sv;
    supplied_type = Web::MimeSniff::MimeType::parse("text/html"sv).release_value();
    computed_mime_type = Web::MimeSniff::Resource::sniff(""sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                           .sniffing_context = Web::MimeSniff::SniffingContext::Font,
                                                                           .supplied_type = supplied_type,
                                                                       });

    EXPECT_EQ(mime_type, computed_mime_type.essence());
}

TEST_CASE(determine_computed_mime_type_given_text_or_binary_context)
{
    auto supplied_type = Web::MimeSniff::MimeType::create("text"_string, "plain"_string);
    auto computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                                    .sniffing_context = Web::MimeSniff::SniffingContext::TextOrBinary,
                                                                                    .supplied_type = supplied_type,
                                                                                });
    EXPECT_EQ("application/octet-stream"sv, computed_mime_type.serialized());
}

TEST_CASE(determine_minimised_mime_type)
{
    HashMap<StringView, StringView> mime_type_to_minimised_mime_type_map;

    // JavaScript MIME types should always be "text/javascript".
    mime_type_to_minimised_mime_type_map.set("text/javascript"sv, "text/javascript"sv);
    mime_type_to_minimised_mime_type_map.set("application/javascript"sv, "text/javascript"sv);
    mime_type_to_minimised_mime_type_map.set("text/javascript; charset=utf-8"sv, "text/javascript"sv);

    // JSON MIME types should always be "application/json".
    mime_type_to_minimised_mime_type_map.set("application/json"sv, "application/json"sv);
    mime_type_to_minimised_mime_type_map.set("text/json"sv, "application/json"sv);
    mime_type_to_minimised_mime_type_map.set("application/json; charset=utf-8"sv, "application/json"sv);

    // SVG MIME types should always be "image/svg+xml".
    mime_type_to_minimised_mime_type_map.set("image/svg+xml"sv, "image/svg+xml"sv);
    mime_type_to_minimised_mime_type_map.set("image/svg+xml; charset=utf-8"sv, "image/svg+xml"sv);

    // XML MIME types should always be "application/xml".
    mime_type_to_minimised_mime_type_map.set("application/xml"sv, "application/xml"sv);
    mime_type_to_minimised_mime_type_map.set("text/xml"sv, "application/xml"sv);
    mime_type_to_minimised_mime_type_map.set("application/xml; charset=utf-8"sv, "application/xml"sv);

    // MIME types not supported by the user-agent should return an empty string.
    mime_type_to_minimised_mime_type_map.set("application/java-archive"sv, ""sv);
    mime_type_to_minimised_mime_type_map.set("application/zip"sv, ""sv);

    for (auto const& mime_type_to_minimised_mime_type : mime_type_to_minimised_mime_type_map) {
        auto mime_type = Web::MimeSniff::MimeType::parse(mime_type_to_minimised_mime_type.key).release_value();
        EXPECT_EQ(mime_type_to_minimised_mime_type.value, Web::MimeSniff::minimise_a_supported_mime_type(mime_type));
    }
}
