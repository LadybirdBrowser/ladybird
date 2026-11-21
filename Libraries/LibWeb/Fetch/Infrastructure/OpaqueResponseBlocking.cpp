/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/NoSniffBlocking.h>
#include <LibWeb/Fetch/Infrastructure/OpaqueResponseBlocking.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWebCommon/MimeSniff/MimeType.h>
#include <LibWebCommon/RustFFI.h>

namespace Web::Fetch::Infrastructure {

// The opaque-response-safelist check reads at most this many bytes of a response's body.
static constexpr size_t SAFELIST_CHECK_BYTE_COUNT = 1024;

// https://whatpr.org/fetch/1442.html#opaque-response-safelisted-mime-type
static bool is_opaque_response_safelisted_mime_type(MimeSniff::MimeType const& mime_type)
{
    // An opaque-response-safelisted MIME type is a JavaScript MIME type or a MIME type whose essence is "text/css" or
    // "image/svg+xml".
    if (mime_type.is_javascript() || mime_type.essence().is_one_of("text/css"sv, "image/svg+xml"sv))
        return true;

    // AD-HOC: Also take the image formats we decode that the image type pattern matching algorithm doesn't recognize,
    //         which the check would otherwise block for not sniffing as an image.
    return mime_type.essence().is_one_of("image/avif"sv, "image/jxl"sv);
}

// https://whatpr.org/fetch/1442.html#opaque-response-blocklisted-mime-type
static bool is_opaque_response_blocklisted_mime_type(MimeSniff::MimeType const& mime_type)
{
    // An opaque-response-blocklisted MIME type is an HTML MIME type, JSON MIME type, or XML MIME type.
    return mime_type.is_html() || mime_type.is_json() || mime_type.is_xml();
}

// https://whatpr.org/fetch/1442.html#opaque-response-blocklisted-never-sniffed-mime-type
static bool is_opaque_response_blocklisted_never_sniffed_mime_type(MimeSniff::MimeType const& mime_type)
{
    // An opaque-response-blocklisted-never-sniffed MIME type is a MIME type whose essence is one of:
    return mime_type.essence().is_one_of(
        "application/dash+xml"sv,
        "application/gzip"sv,
        "application/msexcel"sv,
        "application/mspowerpoint"sv,
        "application/msword"sv,
        "application/msword-template"sv,
        "application/pdf"sv,
        "application/vnd.apple.mpegurl"sv,
        "application/vnd.ces-quickpoint"sv,
        "application/vnd.ces-quicksheet"sv,
        "application/vnd.ces-quickword"sv,
        "application/vnd.ms-excel"sv,
        "application/vnd.ms-excel.sheet.macroenabled.12"sv,
        "application/vnd.ms-powerpoint"sv,
        "application/vnd.ms-powerpoint.presentation.macroenabled.12"sv,
        "application/vnd.ms-word"sv,
        "application/vnd.ms-word.document.12"sv,
        "application/vnd.ms-word.document.macroenabled.12"sv,
        "application/vnd.msword"sv,
        "application/vnd.openxmlformats-officedocument.presentationml.presentation"sv,
        "application/vnd.openxmlformats-officedocument.presentationml.template"sv,
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"sv,
        "application/vnd.openxmlformats-officedocument.spreadsheetml.template"sv,
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document"sv,
        "application/vnd.openxmlformats-officedocument.wordprocessingml.template"sv,
        "application/vnd.presentation-openxml"sv,
        "application/vnd.presentation-openxmlm"sv,
        "application/vnd.spreadsheet-openxml"sv,
        "application/vnd.wordprocessing-openxml"sv,
        "application/x-gzip"sv,
        "application/x-protobuf"sv,
        "application/x-protobuffer"sv,
        "application/zip"sv,
        "audio/mpegurl"sv,
        "multipart/byteranges"sv,
        "multipart/signed"sv,
        "text/event-stream"sv,
        "text/csv"sv,
        "text/vtt"sv);
}

// https://wicg.github.io/background-fetch/#validate-a-partial-response, given 0 as the expected range start: Whether
// response is a partial response for a range starting at the beginning of the resource.
static bool is_partial_response_from_the_start(Response const& response)
{
    auto values = response.header_list()->extract_content_range_values();
    if (values.has<HTTP::HeaderList::ExtractContentRangeFailure>())
        return false;
    return values.get<HTTP::HeaderList::ContentRangeValues>().first_byte_pos == 0;
}

bool opaque_response_safelist_check_applies(Request const& request, Response const& response)
{
    // If request’s response tainting is "opaque", response’s status is not a redirect status, and the
    // opaque-response-safelist check given request and response returns false, then return a network error.
    // https://whatpr.org/fetch/1442.html#http-fetch
    if (request.response_tainting() != Request::ResponseTainting::Opaque || response.is_network_error() || is_redirect_status(response.status()))
        return false;

    // AD-HOC: Media elements don't keep the no-cors media request state that the check tells their range requests apart
    //         by, so their responses aren't checked.
    if (request.destination().has_value() && first_is_one_of(*request.destination(), Request::Destination::Audio, Request::Destination::Track, Request::Destination::Video))
        return false;

    // AD-HOC: Nor are those of prefetches, whose responses no page reads, and whose load event fires for any response
    //         that isn't a network error, an HTTP error status included.
    return request.initiator() != Request::Initiator::Prefetch;
}

// https://whatpr.org/fetch/1442.html#opaque-response-safelist-check
Optional<bool> opaque_response_safelist_check_before_reading_bytes(Response const& response)
{
    // 1. Let mimeType be the result of extracting a MIME type from response’s header list.
    auto mime_type = extract_mime_type(response.header_list());

    // 2. Let nosniff be the result of determining nosniff given response’s header list.
    auto nosniff = determine_nosniff(response.header_list());

    // 3. If mimeType is not failure, then:
    if (mime_type.has_value()) {
        // 1. If mimeType is an opaque-response-safelisted MIME type, then return true.
        if (is_opaque_response_safelisted_mime_type(*mime_type))
            return true;

        // 2. If mimeType is an opaque-response-blocklisted-never-sniffed MIME type, then return false.
        if (is_opaque_response_blocklisted_never_sniffed_mime_type(*mime_type))
            return false;

        // 3. If response’s status is 206 and mimeType is an opaque-response-blocklisted MIME type, then return false.
        if (response.status() == 206 && is_opaque_response_blocklisted_mime_type(*mime_type))
            return false;

        // 4. If nosniff is true and mimeType is an opaque-response-blocklisted MIME type or its essence is
        //    "text/plain", then return false.
        if (nosniff && (is_opaque_response_blocklisted_mime_type(*mime_type) || mime_type->essence() == "text/plain"sv))
            return false;
    }

    // 4. If request’s no-cors media request state is "subsequent", then return true.
    // NB: It is "N/A" for every request checked (see opaque_response_safelist_check_applies()).

    // 5. If response’s status is 206 and validate a partial response given 0 and response returns invalid, then
    //    return false.
    if (response.status() == 206 && !is_partial_response_from_the_start(response))
        return false;

    // NB: Steps 6 onward run in opaque_response_safelist_check_given_bytes().
    return {};
}

// https://whatpr.org/fetch/1442.html#opaque-response-safelist-check
bool opaque_response_safelist_check_given_bytes(Response const& response, Optional<ReadonlyBytes> resource_header)
{
    // 6. Let bytes be the result of running obtain a copy of the first 1024 bytes of response given response.
    // 7. If bytes is failure, then return false.
    if (!resource_header.has_value())
        return false;
    auto bytes = resource_header->trim(SAFELIST_CHECK_BYTE_COUNT);

    // 8. If the audio or video type pattern matching algorithm given bytes does not return undefined, then:
    if (FFI::mime_sniff_matches_an_audio_or_video_type_pattern(bytes)) {
        // 1. If request’s no-cors media request state is not "initial", then return false.
        // NB: It is "N/A" for every request checked.
        return false;
    }

    // 9. If request’s no-cors media request state is not "N/A", then return false.
    // NB: It is "N/A" for every request checked.

    // 10. If the image type pattern matching algorithm given bytes does not return undefined, then return true.
    if (FFI::mime_sniff_matches_an_image_type_pattern(bytes))
        return true;

    // 11. If nosniff is true, then return false.
    if (determine_nosniff(response.header_list()))
        return false;

    // 12. If response’s status is not an ok status, then return false.
    if (!is_ok_status(response.status()))
        return false;

    // 13. If mimeType is failure, then return true.
    auto mime_type = extract_mime_type(response.header_list());
    if (!mime_type.has_value())
        return true;

    // 14. If mimeType’s essence starts with "audio/", "image/", or "video/", then return false.
    if (mime_type->essence().starts_with_bytes("audio/"sv) || mime_type->essence().starts_with_bytes("image/"sv) || mime_type->essence().starts_with_bytes("video/"sv))
        return false;

    // 15. Return determine if response is JavaScript and not JSON given response.
    // AD-HOC: Rather than parse the whole body as JavaScript, block a response only where its first bytes show it isn't
    //         a script that runs: HTML, XML, a JSON object, or a prefix that stops a script. Every other response is
    //         allowed, as it may be one.
    return !FFI::mime_sniff_shows_a_resource_that_is_not_a_script(bytes);
}

}
