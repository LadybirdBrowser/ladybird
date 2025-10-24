/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <stddef.h>
#include <stdint.h>

// Fuzz IPC message deserialization to find crashes and undefined behavior
extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    // Early exit for invalid sizes
    if (size == 0 || size > 16 * 1024 * 1024) // 16 MiB limit
        return 0;

    // Create buffer from fuzzer input
    auto buffer_or_error = ByteBuffer::copy(data, size);
    if (buffer_or_error.is_error())
        return 0;

    auto buffer = buffer_or_error.release_value();

    // Try to decode as IPC message
    IPC::Decoder decoder(move(buffer));

    // Fuzz primitive integer types
    {
        [[maybe_unused]] auto _ = decoder.decode<u8>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<u16>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<u32>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<u64>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<i8>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<i16>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<i32>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<i64>();
    }

    // Fuzz boolean type
    {
        [[maybe_unused]] auto _ = decoder.decode<bool>();
    }

    // Fuzz floating point types
    {
        [[maybe_unused]] auto _ = decoder.decode<float>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<double>();
    }

    // Fuzz string types
    {
        [[maybe_unused]] auto _ = decoder.decode<String>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<ByteString>();
    }

    // Fuzz buffer types
    {
        [[maybe_unused]] auto _ = decoder.decode<ByteBuffer>();
    }

    // Fuzz vector types (common in IPC)
    {
        [[maybe_unused]] auto _ = decoder.decode<Vector<u8>>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<Vector<u16>>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<Vector<u32>>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<Vector<u64>>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<Vector<String>>();
    }

    // Fuzz hash map types
    {
        [[maybe_unused]] auto _ = decoder.decode<HashMap<String, String>>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<HashMap<u32, String>>();
    }

    // Fuzz optional types
    {
        [[maybe_unused]] auto _ = decoder.decode<Optional<String>>();
    }
    {
        [[maybe_unused]] auto _ = decoder.decode<Optional<u32>>();
    }

    return 0;
}

// Fuzzer initialization (optional)
extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    return 0;
}
