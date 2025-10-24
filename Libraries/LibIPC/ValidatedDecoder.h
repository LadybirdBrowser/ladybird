/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Decoder.h>
#include <LibIPC/Limits.h>
#include <LibIPC/SafeMath.h>

namespace IPC {

// Validated decoding helpers that add bounds checking and validation
// Use these when decoding data from untrusted sources (e.g., WebContent process)
//
// Example:
//   void ConnectionFromClient::handle_message(IPC::Decoder& decoder)
//   {
//       // Decode with validation
//       auto url = TRY(ValidatedDecoder::decode_url(decoder));
//       auto buffer = TRY(ValidatedDecoder::decode_byte_buffer(decoder));
//   }

class ValidatedDecoder {
public:
    // Validated String decoding with size limit
    static ErrorOr<String> decode_string(Decoder& decoder)
    {
        auto string = TRY(decoder.decode<String>());

        // Validate string length
        if (string.bytes_as_string_view().length() > Limits::MaxStringLength)
            return Error::from_string_literal("String exceeds maximum length");

        return string;
    }

    // Validated ByteString decoding with size limit
    static ErrorOr<ByteString> decode_byte_string(Decoder& decoder)
    {
        auto string = TRY(decoder.decode<ByteString>());

        // Validate string length
        if (string.length() > Limits::MaxStringLength)
            return Error::from_string_literal("ByteString exceeds maximum length");

        return string;
    }

    // Validated ByteBuffer decoding with size limit
    static ErrorOr<ByteBuffer> decode_byte_buffer(Decoder& decoder)
    {
        auto buffer = TRY(decoder.decode<ByteBuffer>());

        // Validate buffer size
        if (buffer.size() > Limits::MaxByteBufferSize)
            return Error::from_string_literal("ByteBuffer exceeds maximum size");

        return buffer;
    }

    // Validated Vector decoding with size limit
    template<typename T>
    static ErrorOr<Vector<T>> decode_vector(Decoder& decoder)
    {
        auto vector = TRY(decoder.decode<Vector<T>>());

        // Validate vector size
        if (vector.size() > Limits::MaxVectorSize)
            return Error::from_string_literal("Vector exceeds maximum size");

        return vector;
    }

    // Validated HashMap decoding with size limit
    template<typename K, typename V>
    static ErrorOr<HashMap<K, V>> decode_hash_map(Decoder& decoder)
    {
        auto map = TRY(decoder.decode<HashMap<K, V>>());

        // Validate map size
        if (map.size() > Limits::MaxHashMapSize)
            return Error::from_string_literal("HashMap exceeds maximum size");

        return map;
    }

    // Validated URL decoding with length limit
    static ErrorOr<URL::URL> decode_url(Decoder& decoder)
    {
        auto url = TRY(decoder.decode<URL::URL>());

        // Validate URL length
        if (url.to_string().bytes_as_string_view().length() > Limits::MaxURLLength)
            return Error::from_string_literal("URL exceeds maximum length");

        return url;
    }

    // Validated image dimensions with overflow protection
    struct ImageDimensions {
        u32 width;
        u32 height;
        u32 bytes_per_pixel;
        size_t buffer_size;
    };

    static ErrorOr<ImageDimensions> decode_image_dimensions(Decoder& decoder)
    {
        auto width = TRY(decoder.decode<u32>());
        auto height = TRY(decoder.decode<u32>());
        auto bytes_per_pixel = TRY(decoder.decode<u32>());

        // Validate dimensions are within reasonable limits
        if (width > Limits::MaxImageWidth)
            return Error::from_string_literal("Image width exceeds maximum");

        if (height > Limits::MaxImageHeight)
            return Error::from_string_literal("Image height exceeds maximum");

        if (bytes_per_pixel == 0 || bytes_per_pixel > 16)
            return Error::from_string_literal("Invalid bytes per pixel");

        // Calculate buffer size with overflow protection
        auto buffer_size = TRY(SafeMath::calculate_buffer_size(width, height, bytes_per_pixel));

        // Validate buffer size doesn't exceed limit
        if (buffer_size > Limits::MaxByteBufferSize)
            return Error::from_string_literal("Image buffer size exceeds maximum");

        return ImageDimensions {
            .width = width,
            .height = height,
            .bytes_per_pixel = bytes_per_pixel,
            .buffer_size = buffer_size
        };
    }

    // Validated coordinate decoding (for mouse events, etc.)
    struct Point {
        i32 x;
        i32 y;
    };

    static ErrorOr<Point> decode_point(Decoder& decoder)
    {
        auto x = TRY(decoder.decode<i32>());
        auto y = TRY(decoder.decode<i32>());

        // Coordinates should be within reasonable screen bounds
        // Even large displays are < 32K pixels
        constexpr i32 max_coordinate = 32768;
        constexpr i32 min_coordinate = -32768;

        if (x < min_coordinate || x > max_coordinate)
            return Error::from_string_literal("X coordinate out of valid range");

        if (y < min_coordinate || y > max_coordinate)
            return Error::from_string_literal("Y coordinate out of valid range");

        return Point { .x = x, .y = y };
    }

    // Validated size decoding (for viewport, window size, etc.)
    struct Size {
        u32 width;
        u32 height;
    };

    static ErrorOr<Size> decode_size(Decoder& decoder)
    {
        auto width = TRY(decoder.decode<u32>());
        auto height = TRY(decoder.decode<u32>());

        // Validate dimensions are reasonable
        if (width == 0 || width > Limits::MaxImageWidth)
            return Error::from_string_literal("Width out of valid range");

        if (height == 0 || height > Limits::MaxImageHeight)
            return Error::from_string_literal("Height out of valid range");

        return Size { .width = width, .height = height };
    }

    // Validated HTTP header decoding
    static ErrorOr<HashMap<String, String>> decode_http_headers(Decoder& decoder)
    {
        auto headers = TRY(decoder.decode<HashMap<String, String>>());

        // Validate header count
        if (headers.size() > Limits::MaxHTTPHeaderCount)
            return Error::from_string_literal("Too many HTTP headers");

        // Validate each header value size
        for (auto const& [name, value] : headers) {
            if (value.bytes_as_string_view().length() > Limits::MaxHTTPHeaderValueSize)
                return Error::from_string_literal("HTTP header value too large");
        }

        return headers;
    }

    // Validated cookie decoding
    static ErrorOr<String> decode_cookie(Decoder& decoder)
    {
        auto cookie = TRY(decoder.decode<String>());

        // Validate cookie size (RFC 6265)
        if (cookie.bytes_as_string_view().length() > Limits::MaxCookieSize)
            return Error::from_string_literal("Cookie exceeds maximum size");

        return cookie;
    }

    // Validated page ID decoding (prevent using invalid IDs)
    template<typename PageMap>
    static ErrorOr<u64> decode_page_id(Decoder& decoder, PageMap const& valid_pages)
    {
        auto page_id = TRY(decoder.decode<u64>());

        // Validate page ID exists in the map
        if (!valid_pages.contains(page_id))
            return Error::from_string_literal("Invalid page ID");

        return page_id;
    }

    // Validated index decoding with bounds checking
    static ErrorOr<size_t> decode_index(Decoder& decoder, size_t max_value)
    {
        auto index = TRY(decoder.decode_size());

        if (index >= max_value)
            return Error::from_string_literal("Index out of bounds");

        return index;
    }

    // Validated offset/length pair for buffer operations
    struct Range {
        size_t offset;
        size_t length;
    };

    static ErrorOr<Range> decode_range(Decoder& decoder, size_t buffer_size)
    {
        auto offset = TRY(decoder.decode_size());
        auto length = TRY(decoder.decode_size());

        // Validate offset is within buffer
        TRY(SafeMath::validate_index(offset, buffer_size));

        // Validate range doesn't exceed buffer
        TRY(SafeMath::validate_range(offset, offset + length, buffer_size));

        return Range { .offset = offset, .length = length };
    }
};

}
