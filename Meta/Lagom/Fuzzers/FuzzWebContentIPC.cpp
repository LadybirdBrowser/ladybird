/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibIPC/Decoder.h>
#include <LibURL/URL.h>

// Fuzz WebContentServer IPC messages specifically
// These are high-value targets as they cross the trust boundary between
// the UI process (trusted) and WebContent process (untrusted)
extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    if (size == 0 || size > 16 * 1024 * 1024) // 16 MiB limit
        return 0;

    auto buffer_or_error = ByteBuffer::copy(data, size);
    if (buffer_or_error.is_error())
        return 0;

    IPC::Decoder decoder(buffer_or_error.release_value());

    // Fuzz URL parsing (high-value target for security issues)
    // URLs come from untrusted web content and must be validated
    {
        [[maybe_unused]] auto _ = decoder.decode<URL::URL>();
    }

    // Fuzz string inputs (potential XSS, injection vectors)
    // These represent page titles, alert messages, console output, etc.
    {
        [[maybe_unused]] auto _ = decoder.decode<String>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<ByteString>();
    }

    // Fuzz page IDs (could be used for UXSS if not validated)
    // Invalid page IDs could allow cross-origin access
    {
        [[maybe_unused]] auto _ = decoder.decode<u64>();
    }

    // Fuzz mouse/keyboard inputs (input spoofing attacks)
    // Coordinates and modifiers
    {
        [[maybe_unused]] auto x = decoder.decode<i32>();
        [[maybe_unused]] auto y = decoder.decode<i32>();
        [[maybe_unused]] auto button = decoder.decode<u32>();
        [[maybe_unused]] auto modifiers = decoder.decode<u32>();
    }

    // Fuzz buffer inputs (image data, fetch responses, file uploads)
    // These can trigger parser vulnerabilities
    {
        [[maybe_unused]] auto _ = decoder.decode<ByteBuffer>();
    }

    // Fuzz vector of buffers (multipart form data, etc.)
    {
        [[maybe_unused]] auto _ = decoder.decode<Vector<ByteBuffer>>();
    }

    // Fuzz complex types (HTTP headers, cookies, localStorage)
    {
        [[maybe_unused]] auto _ = decoder.decode<HashMap<String, String>>();
    }

    // Fuzz vector of strings (command line arguments, form field names)
    {
        [[maybe_unused]] auto _ = decoder.decode<Vector<String>>();
    }

    // Fuzz dimensions (viewport size, image dimensions)
    // Can trigger integer overflow in size calculations
    {
        [[maybe_unused]] auto width = decoder.decode<u32>();
        [[maybe_unused]] auto height = decoder.decode<u32>();
    }

    // Fuzz nested structures (JSON-like data)
    {
        [[maybe_unused]] auto _ = decoder.decode<Vector<Vector<String>>>();
    }

    // Fuzz optional types (may be null/none)
    {
        [[maybe_unused]] auto _ = decoder.decode<Optional<URL::URL>>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<Optional<ByteBuffer>>();
    }

    return 0;
}

// Fuzzer initialization
extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    return 0;
}
