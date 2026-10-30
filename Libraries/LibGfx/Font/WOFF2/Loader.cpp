/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Checked.h>
#include <AK/MemoryStream.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <woff2/decode.h>

namespace WOFF2 {

// NB: We refuse to decode a WOFF2 font into more than this many bytes.
static constexpr size_t MAX_OUTPUT_SIZE = 30 * MiB;

// https://www.w3.org/TR/WOFF2/#DataTypes
static ErrorOr<u32> read_uint_base_128(Stream& stream)
{
    u32 accumulator = 0;
    for (size_t i = 0; i < 5; ++i) {
        auto data_byte = TRY(stream.read_value<u8>());
        if (i == 0 && data_byte == 0x80)
            return Error::from_string_literal("UIntBase128 with leading zeros");
        if (accumulator & 0xFE000000)
            return Error::from_string_literal("UIntBase128 overflow");
        accumulator = (accumulator << 7) | (data_byte & 0x7F);
        if ((data_byte & 0x80) == 0)
            return accumulator;
    }
    return Error::from_string_literal("UIntBase128 longer than 5 bytes");
}

// NB: The woff2 decoder allocates one buffer for all of the Brotli-decompressed table data before it writes any
//     output. That size is the sum of the table lengths in the table directory, which we compute here the same way
//     woff2 does, so that we can refuse a font before that allocation.
//     https://www.w3.org/TR/WOFF2/#table_dir_format
static ErrorOr<u64> compute_decompressed_table_data_size(ReadonlyBytes bytes)
{
    static constexpr size_t header_size = 48;
    static constexpr u8 glyf_tag_index = 10;
    static constexpr u8 loca_tag_index = 11;
    static constexpr u8 arbitrary_tag_index = 0x3F;

    FixedMemoryStream stream { bytes };
    TRY(stream.seek(12));
    auto num_tables = TRY(stream.read_value<BigEndian<u16>>());
    TRY(stream.seek(header_size));

    u64 size = 0;
    for (u16 i = 0; i < num_tables; ++i) {
        auto flags = TRY(stream.read_value<u8>());
        u8 tag_index = flags & 0x3F;
        u8 transform_version = flags >> 6;

        bool is_glyf_or_loca = tag_index == glyf_tag_index || tag_index == loca_tag_index;
        if (tag_index == arbitrary_tag_index) {
            Array<u8, 4> tag;
            TRY(stream.read_until_filled(tag));
            auto tag_view = StringView { tag.span() };
            is_glyf_or_loca = tag_view == "glyf"sv || tag_view == "loca"sv;
        }

        // NB: The glyf and loca tables are transformed with version 0, all other tables with any other version. A
        //     transformed table has a transformLength, which is its size in the decompressed data.
        bool is_transformed = is_glyf_or_loca ? transform_version == 0 : transform_version != 0;

        auto length = TRY(read_uint_base_128(stream));
        if (is_transformed)
            length = TRY(read_uint_base_128(stream));
        size += length;
    }
    return size;
}

class WOFF2ByteBufferOut final : public woff2::WOFF2Out {
public:
    explicit WOFF2ByteBufferOut(ByteBuffer& buffer)
        : m_buffer(buffer)
    {
    }

    virtual bool Write(void const* data, size_t size) override
    {
        if (size > MAX_OUTPUT_SIZE - m_buffer.size())
            return false;
        if (m_buffer.try_append(static_cast<u8 const*>(data), size).is_error())
            return false;
        return true;
    }

    virtual bool Write(void const* data, size_t offset, size_t n) override
    {
        if (Checked<size_t>::addition_would_overflow(offset, n))
            return false;
        if (offset + n > MAX_OUTPUT_SIZE)
            return false;
        if (offset + n > m_buffer.size()) {
            if (m_buffer.try_resize(offset + n).is_error())
                return false;
        }
        if (n > 0)
            memcpy(m_buffer.offset_pointer(offset), data, n);
        return true;
    }

    virtual size_t Size() override
    {
        return m_buffer.size();
    }

private:
    ByteBuffer& m_buffer;
};

ErrorOr<Core::AnonymousBuffer> convert_to_ttf(ReadonlyBytes bytes)
{
    if (TRY(compute_decompressed_table_data_size(bytes)) > MAX_OUTPUT_SIZE)
        return Error::from_string_literal("Decompressed WOFF2 table data is too big");

    auto ttf_buffer = TRY(ByteBuffer::create_uninitialized(0));
    auto output = WOFF2ByteBufferOut { ttf_buffer };
    if (!woff2::ConvertWOFF2ToTTF(bytes.data(), bytes.size(), &output))
        return Error::from_string_literal("Failed to convert the WOFF2 font to TTF");

    auto anonymous_buffer = TRY(Core::AnonymousBuffer::create_with_size(ttf_buffer.size()));
    if (!ttf_buffer.is_empty())
        memcpy(anonymous_buffer.data<void>(), ttf_buffer.data(), ttf_buffer.size());
    return anonymous_buffer;
}

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_bytes(ReadonlyBytes bytes)
{
    return Gfx::Typeface::try_load_from_anonymous_buffer(TRY(convert_to_ttf(bytes)));
}

}
