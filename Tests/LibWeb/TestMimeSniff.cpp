/*
 * Copyright (c) 2023-2024, Kemal Zebari <kemalzebra@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/MimeSniff/MimeType.h>

#include <LibWeb/MimeSniff/Resource.h>

TEST_CASE(determine_computed_mime_type_given_no_sniff_is_set)
{
    auto mime_type = Web::MimeSniff::MimeType::create("text"_string, "html"_string);
    auto computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"_sv.bytes(), Web::MimeSniff::SniffingConfiguration { .supplied_type = mime_type, .no_sniff = true });

    EXPECT_EQ("text/html"_sv, computed_mime_type.serialized());

    // Cover the edge case in the context-specific sniffing algorithm.
    computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"_sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                                .sniffing_context = Web::MimeSniff::SniffingContext::Image,
                                                                                .supplied_type = mime_type,
                                                                                .no_sniff = true,
                                                                            });

    EXPECT_EQ("text/html"_sv, computed_mime_type.serialized());
}

TEST_CASE(determine_computed_mime_type_given_no_sniff_is_unset)
{
    auto supplied_type = Web::MimeSniff::MimeType::create("application"_string, "x-this-is-a-test"_string);
    auto computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"_sv.bytes(), Web::MimeSniff::SniffingConfiguration { .supplied_type = supplied_type });

    EXPECT_EQ("application/x-this-is-a-test"_sv, computed_mime_type.serialized());
}

TEST_CASE(determine_computed_mime_type_given_xml_mime_type_as_supplied_type)
{
    auto xml_mime_type = "application/rss+xml"_sv;
    auto supplied_type = Web::MimeSniff::MimeType::parse(xml_mime_type).release_value();
    auto computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"_sv.bytes(), Web::MimeSniff::SniffingConfiguration { .supplied_type = supplied_type });

    EXPECT_EQ(xml_mime_type, computed_mime_type.serialized());
}

static void set_image_type_mappings(HashMap<StringView, Vector<StringView>>& mime_type_to_headers_map)
{
    mime_type_to_headers_map.set("image/x-icon"_sv, { "\x00\x00\x01\x00"_sv, "\x00\x00\x02\x00"_sv });
    mime_type_to_headers_map.set("image/bmp"_sv, { "BM"_sv });
    mime_type_to_headers_map.set("image/gif"_sv, { "GIF87a"_sv, "GIF89a"_sv });
    mime_type_to_headers_map.set("image/webp"_sv, { "RIFF\x00\x00\x00\x00WEBPVP"_sv });
    mime_type_to_headers_map.set("image/png"_sv, { "\x89PNG\x0D\x0A\x1A\x0A"_sv });
    mime_type_to_headers_map.set("image/jpeg"_sv, { "\xFF\xD8\xFF"_sv });
}

static void set_audio_or_video_type_mappings(HashMap<StringView, Vector<StringView>>& mime_type_to_headers_map)
{
    mime_type_to_headers_map.set("audio/aiff"_sv, { "FORM\x00\x00\x00\x00\x41IFF"_sv });
    mime_type_to_headers_map.set("audio/mpeg"_sv, { "ID3"_sv });
    mime_type_to_headers_map.set("application/ogg"_sv, { "OggS\x00"_sv });
    mime_type_to_headers_map.set("audio/midi"_sv, { "MThd\x00\x00\x00\x06"_sv });
    mime_type_to_headers_map.set("video/avi"_sv, { "RIFF\x00\x00\x00\x00\x41\x56\x49\x20"_sv });
    mime_type_to_headers_map.set("audio/wave"_sv, { "RIFF\x00\x00\x00\x00WAVE"_sv });
}

static void set_text_plain_type_mappings(HashMap<StringView, Vector<StringView>>& mime_type_to_headers_map)
{
    mime_type_to_headers_map.set("text/plain"_sv, {
                                                      "\xFE\xFF\x00\x00"_sv,
                                                      "\xFF\xFE\x00\x00"_sv,
                                                      "\xEF\xBB\xBF\x00"_sv,
                                                      "Hello world!"_sv,
                                                  });
}

TEST_CASE(determine_computed_mime_type_given_supplied_type_that_is_an_apache_bug_mime_type)
{
    Vector<StringView> apache_bug_mime_types = {
        "text/plain"_sv,
        "text/plain; charset=ISO-8859-1"_sv,
        "text/plain; charset=iso-8859-1"_sv,
        "text/plain; charset=UTF-8"_sv
    };

    // Cover all Apache bug MIME types.
    for (auto const& apache_bug_mime_type : apache_bug_mime_types) {
        auto supplied_type = Web::MimeSniff::MimeType::parse(apache_bug_mime_type).release_value();
        auto computed_mime_type = Web::MimeSniff::Resource::sniff("Hello world!"_sv.bytes(),
            Web::MimeSniff::SniffingConfiguration { .scheme = "http"_sv, .supplied_type = supplied_type });

        EXPECT_EQ("text/plain"_sv, computed_mime_type.serialized());
    }

    // Cover all code paths in "rules for distinguishing if a resource is text or binary".
    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;
    mime_type_to_headers_map.set("application/octet-stream"_sv, { "\x00"_sv });

    set_text_plain_type_mappings(mime_type_to_headers_map);

    auto supplied_type = Web::MimeSniff::MimeType::create("text"_string, "plain"_string);
    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(),
                Web::MimeSniff::SniffingConfiguration { .scheme = "http"_sv, .supplied_type = supplied_type });

            EXPECT_EQ(mime_type, computed_mime_type.serialized());
        }
    }
}

TEST_CASE(determine_computed_mime_type_given_xml_or_html_supplied_type)
{
    // With HTML supplied type.
    auto config = Web::MimeSniff::SniffingConfiguration { .supplied_type = Web::MimeSniff::MimeType::create("text"_string, "html"_string) };
    auto computed_mime_type = Web::MimeSniff::Resource::sniff(""_sv.bytes(), config);
    EXPECT_EQ("text/html"_sv, computed_mime_type.serialized());

    // With XML supplied type.
    config = Web::MimeSniff::SniffingConfiguration { .supplied_type = Web::MimeSniff::MimeType::create("text"_string, "xml"_string) };
    computed_mime_type = Web::MimeSniff::Resource::sniff(""_sv.bytes(), config);
    EXPECT_EQ("text/xml"_sv, computed_mime_type.serialized());
}

TEST_CASE(determine_computed_mime_type_in_both_none_and_browsing_sniffing_context)
{
    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    mime_type_to_headers_map.set("application/octet-stream"_sv, { "\x00"_sv });
    mime_type_to_headers_map.set("text/html"_sv, {
                                                     "\x09\x09<!DOCTYPE HTML\x20"_sv,
                                                     "\x0A<HTML\x3E"_sv,
                                                     "\x0C<HEAD\x20"_sv,
                                                     "\x0D<SCRIPT>"_sv,
                                                     "\x20<IFRAME>"_sv,
                                                     "<H1>"_sv,
                                                     "<DIV>"_sv,
                                                     "<FONT>"_sv,
                                                     "<TABLE>"_sv,
                                                     "<A>"_sv,
                                                     "<STYLE>"_sv,
                                                     "<TITLE>"_sv,
                                                     "<B>"_sv,
                                                     "<BODY>"_sv,
                                                     "<BR>"_sv,
                                                     "<P>"_sv,
                                                     "<!-->"_sv,
                                                 });
    mime_type_to_headers_map.set("text/xml"_sv, { "<?xml"_sv });
    mime_type_to_headers_map.set("application/pdf"_sv, { "%PDF-"_sv });
    mime_type_to_headers_map.set("application/postscript"_sv, { "%!PS-Adobe-"_sv });

    set_text_plain_type_mappings(mime_type_to_headers_map);
    set_image_type_mappings(mime_type_to_headers_map);
    set_audio_or_video_type_mappings(mime_type_to_headers_map);

    mime_type_to_headers_map.set("application/x-gzip"_sv, { "\x1F\x8B\x08"_sv });
    mime_type_to_headers_map.set("application/zip"_sv, { "PK\x03\x04"_sv });
    mime_type_to_headers_map.set("application/x-rar-compressed"_sv, { "Rar\x20\x1A\x07\x00"_sv });

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
    auto header_bytes = "<HTML>"_sv.bytes();

    for (auto const& unknown_supplied_type : unknown_supplied_types) {
        auto computed_mime_type = Web::MimeSniff::Resource::sniff(header_bytes, Web::MimeSniff::SniffingConfiguration { .supplied_type = unknown_supplied_type });
        EXPECT_EQ("text/html"_sv, computed_mime_type.essence());
    }
}

TEST_CASE(determine_computed_mime_type_in_image_sniffing_context)
{
    // Cover case where supplied type is an XML MIME type.
    auto mime_type = "application/rss+xml"_sv;
    auto supplied_type = Web::MimeSniff::MimeType::parse(mime_type).release_value();
    auto computed_mime_type = Web::MimeSniff::Resource::sniff(""_sv.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Image, .supplied_type = supplied_type });

    EXPECT_EQ(mime_type, computed_mime_type.serialized());

    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    set_image_type_mappings(mime_type_to_headers_map);

    // Also consider a resource that is not an image.
    mime_type_to_headers_map.set("application/octet-stream"_sv, { "\x00"_sv });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Image });
            EXPECT_EQ(mime_type, computed_mime_type.essence());
        }
    }

    // Cover case where we aren't dealing with an image MIME type.
    mime_type = "text/html"_sv;
    supplied_type = Web::MimeSniff::MimeType::parse("text/html"_sv).release_value();
    computed_mime_type = Web::MimeSniff::Resource::sniff(""_sv.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Image, .supplied_type = supplied_type });

    EXPECT_EQ(mime_type, computed_mime_type.essence());
}

TEST_CASE(determine_computed_mime_type_in_audio_or_video_sniffing_context)
{
    // Cover case where supplied type is an XML MIME type.
    auto mime_type = "application/rss+xml"_sv;
    auto supplied_type = Web::MimeSniff::MimeType::parse(mime_type).release_value();
    auto computed_mime_type = Web::MimeSniff::Resource::sniff(""_sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                                 .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo,
                                                                                 .supplied_type = supplied_type,
                                                                             });

    EXPECT_EQ(mime_type, computed_mime_type.serialized());
    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    set_audio_or_video_type_mappings(mime_type_to_headers_map);

    // Also consider a resource that is not an audio or video.
    mime_type_to_headers_map.set("application/octet-stream"_sv, { "\x00"_sv });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo });
            EXPECT_EQ(mime_type, computed_mime_type.essence());
        }
    }

    // Cover case where we aren't dealing with an audio or video MIME type.
    mime_type = "text/html"_sv;
    supplied_type = Web::MimeSniff::MimeType::parse("text/html"_sv).release_value();
    computed_mime_type = Web::MimeSniff::Resource::sniff(""_sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                            .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo,
                                                                            .supplied_type = supplied_type,
                                                                        });

    EXPECT_EQ(mime_type, computed_mime_type.essence());
}

TEST_CASE(determine_computed_mime_type_when_trying_to_match_mp4_signature)
{
    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    mime_type_to_headers_map.set("application/octet-stream"_sv, {
                                                                    // Payload length < 12.
                                                                    "!= 12"_sv,
                                                                    // Payload length < box size.
                                                                    "\x00\x00\x00\x1F\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A"_sv,
                                                                    // Box size % 4 != 0.
                                                                    "\x00\x00\x00\x0D\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"_sv,
                                                                    // 4 bytes after box size header != "ftyp".
                                                                    "\x00\x00\x00\x0C\x00\x00\x00\x00\x00\x00\x00\x00"_sv,
                                                                    // Sequence "mp4" couldn't be found in ftyp box.
                                                                    "\x00\x00\x00\x18\x66\x74\x79\x70isom\x00\x00\x00\x00\x61\x76\x63\x31\x00\x00\x00\x00"_sv,
                                                                });
    mime_type_to_headers_map.set("video/mp4"_sv, {
                                                     // 3 bytes after "ftyp" sequence == "mp4".
                                                     "\x00\x00\x00\x0C\x66\x74\x79\x70mp42"_sv,
                                                     // "mp4" sequence found while executing while loop (this input covers entire loop)
                                                     "\x00\x00\x00\x18\x66\x74\x79\x70isom\x00\x00\x00\x00\x61\x76\x63\x31mp41"_sv,
                                                 });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo });
            EXPECT_EQ(mime_type, computed_mime_type.serialized());
        }
    }
}

TEST_CASE(determine_computed_mime_type_when_trying_to_match_webm_signature)
{

    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;

    mime_type_to_headers_map.set("application/octet-stream"_sv, {
                                                                    // Payload length < 4.
                                                                    "<4"_sv,
                                                                    // First four bytes are not 0x1A 0x45 0xDF 0xA3.
                                                                    "\x00\x00\x00\x00"_sv,
                                                                    // Correct first four bytes, but no following WebM element
                                                                    "\x1A\x45\xDF\xA3\x00\x00\x00\x00"_sv,
                                                                });
    mime_type_to_headers_map.set("video/webm"_sv, {
                                                      // Input that should parse correctly.
                                                      "\x1A\x45\xDF\xA3\x42\x82\x84\x77\x65\x62\x6D\x00"_sv,
                                                  });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::AudioOrVideo });
            EXPECT_EQ(mime_type, computed_mime_type.serialized());
        }
    }
}

// http://mpgedit.org/mpgedit/mpeg_format/mpeghdr.htm
struct MP3FrameOptions {
    bool validLength = true;
    // include the 0xFFF sync word?
    bool sync = true;
    // 3=MPEG-1, 2=MPEG-2, 0=MPEG-2.5
    u8 version = 3;
    // 1=III, 2=II, 3=I
    u8 layer = 1;
    // true=no CRC, false=CRC follows
    bool protect = true;
    // 1–14 valid
    u8 bitrate_index = 9;
    // 0=44.1k,1=48k,2=32k
    u8 samplerate_index = 0;
    // padding bit
    bool padded = false;
    // filler bytes
    size_t payload_bytes = 100;
};

static ByteBuffer make_mp3_frame(MP3FrameOptions opts)
{
    if (!opts.validLength)
        return MUST(ByteBuffer::create_zeroed(2));

    size_t total_size = 4 + opts.payload_bytes;
    auto buffer = MUST(ByteBuffer::create_zeroed(total_size));
    auto* data = buffer.data();

    // first 8 bits of sync (0xFFF)
    if (opts.sync)
        data[0] = 0xFF;

    // 1110 0000 = last three sync bits
    data[1] = 0xE0
        // bits 4–3: version
        | ((opts.version & 0x3) << 3)
        // bits 2–1: layer
        | ((opts.layer & 0x3) << 1)
        // bit 0:  protection
        | (opts.protect & 0x1);

    // bits 7–4: bitrate index
    data[2] = ((opts.bitrate_index & 0xF) << 4)
        // bits 3–2: samplerate index
        | ((opts.samplerate_index & 0x3) << 2)
        // bit 1: pad
        | ((opts.padded & 0x1) << 1);
    // bit 0: private (keep zero)

    // 3) Rest of header (channel flags, etc.) – not needed for sniff
    data[3] = 0x00;

    // Payload bytes are already zeroed

    return buffer;
}

TEST_CASE(determine_computed_mime_type_when_trying_to_match_mp3_no_id3_signature)
{

    HashMap<StringView, Vector<ByteBuffer>> mime_type_to_headers_map;
    mime_type_to_headers_map.set("application/octet-stream"_sv, {
                                                                    // Payload length < 4.
                                                                    make_mp3_frame({ .validLength = false }),
                                                                    // invalid sync
                                                                    make_mp3_frame({ .sync = false }),
                                                                    // invalid layer (reserved)
                                                                    make_mp3_frame({ .layer = 0 }),
                                                                    // invalid bitrate
                                                                    make_mp3_frame({ .bitrate_index = 15 }),
                                                                    // invalid sample rate
                                                                    make_mp3_frame({ .samplerate_index = 3 }),
                                                                });
    mime_type_to_headers_map.set("audio/mpeg"_sv, {
                                                      make_mp3_frame({ .padded = true }),
                                                      make_mp3_frame({ .padded = false }),
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
    auto mime_type = "application/rss+xml"_sv;
    auto supplied_type = Web::MimeSniff::MimeType::parse(mime_type).release_value();
    auto computed_mime_type = Web::MimeSniff::Resource::sniff(""_sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                                 .sniffing_context = Web::MimeSniff::SniffingContext::Font,
                                                                                 .supplied_type = supplied_type,
                                                                             });

    EXPECT_EQ(mime_type, computed_mime_type.serialized());

    HashMap<StringView, Vector<StringView>> mime_type_to_headers_map;
    mime_type_to_headers_map.set("application/octet-stream"_sv, { "\x00"_sv });
    mime_type_to_headers_map.set("application/vnd.ms-fontobject"_sv, { "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00LP"_sv });
    mime_type_to_headers_map.set("font/ttf"_sv, { "\x00\x01\x00\x00"_sv });
    mime_type_to_headers_map.set("font/otf"_sv, { "OTTO"_sv });
    mime_type_to_headers_map.set("font/collection"_sv, { "ttcf"_sv });
    mime_type_to_headers_map.set("font/woff"_sv, { "wOFF"_sv });
    mime_type_to_headers_map.set("font/woff2"_sv, { "wOF2"_sv });

    for (auto const& mime_type_to_headers : mime_type_to_headers_map) {
        auto mime_type = mime_type_to_headers.key;

        for (auto const& header : mime_type_to_headers.value) {
            auto computed_mime_type = Web::MimeSniff::Resource::sniff(header.bytes(), Web::MimeSniff::SniffingConfiguration { .sniffing_context = Web::MimeSniff::SniffingContext::Font });
            EXPECT_EQ(mime_type, computed_mime_type.essence());
        }
    }

    // Cover case where we aren't dealing with a font MIME type.
    mime_type = "text/html"_sv;
    supplied_type = Web::MimeSniff::MimeType::parse("text/html"_sv).release_value();
    computed_mime_type = Web::MimeSniff::Resource::sniff(""_sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                            .sniffing_context = Web::MimeSniff::SniffingContext::Font,
                                                                            .supplied_type = supplied_type,
                                                                        });

    EXPECT_EQ(mime_type, computed_mime_type.essence());
}

TEST_CASE(determine_computed_mime_type_given_text_or_binary_context)
{
    auto supplied_type = Web::MimeSniff::MimeType::create("text"_string, "plain"_string);
    auto computed_mime_type = Web::MimeSniff::Resource::sniff("\x00"_sv.bytes(), Web::MimeSniff::SniffingConfiguration {
                                                                                     .sniffing_context = Web::MimeSniff::SniffingContext::TextOrBinary,
                                                                                     .supplied_type = supplied_type,
                                                                                 });
    EXPECT_EQ("application/octet-stream"_sv, computed_mime_type.serialized());
}

TEST_CASE(determine_minimised_mime_type)
{
    HashMap<StringView, StringView> mime_type_to_minimised_mime_type_map;

    // JavaScript MIME types should always be "text/javascript".
    mime_type_to_minimised_mime_type_map.set("text/javascript"_sv, "text/javascript"_sv);
    mime_type_to_minimised_mime_type_map.set("application/javascript"_sv, "text/javascript"_sv);
    mime_type_to_minimised_mime_type_map.set("text/javascript; charset=utf-8"_sv, "text/javascript"_sv);

    // JSON MIME types should always be "application/json".
    mime_type_to_minimised_mime_type_map.set("application/json"_sv, "application/json"_sv);
    mime_type_to_minimised_mime_type_map.set("text/json"_sv, "application/json"_sv);
    mime_type_to_minimised_mime_type_map.set("application/json; charset=utf-8"_sv, "application/json"_sv);

    // SVG MIME types should always be "image/svg+xml".
    mime_type_to_minimised_mime_type_map.set("image/svg+xml"_sv, "image/svg+xml"_sv);
    mime_type_to_minimised_mime_type_map.set("image/svg+xml; charset=utf-8"_sv, "image/svg+xml"_sv);

    // XML MIME types should always be "application/xml".
    mime_type_to_minimised_mime_type_map.set("application/xml"_sv, "application/xml"_sv);
    mime_type_to_minimised_mime_type_map.set("text/xml"_sv, "application/xml"_sv);
    mime_type_to_minimised_mime_type_map.set("application/xml; charset=utf-8"_sv, "application/xml"_sv);

    // MIME types not supported by the user-agent should return an empty string.
    mime_type_to_minimised_mime_type_map.set("application/java-archive"_sv, ""_sv);
    mime_type_to_minimised_mime_type_map.set("application/zip"_sv, ""_sv);

    for (auto const& mime_type_to_minimised_mime_type : mime_type_to_minimised_mime_type_map) {
        auto mime_type = Web::MimeSniff::MimeType::parse(mime_type_to_minimised_mime_type.key).release_value();
        EXPECT_EQ(mime_type_to_minimised_mime_type.value, Web::MimeSniff::minimise_a_supported_mime_type(mime_type));
    }
}
