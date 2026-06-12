/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibGfx/ImageFormats/ExifReader.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>

namespace Gfx::TIFF {

namespace {

class ExifReader {
public:
    explicit ExifReader(FixedMemoryStream stream)
        : m_stream(move(stream))
    {
    }

    ErrorOr<NonnullOwnPtr<ExifMetadata>> read_metadata()
    {
        TRY(read_image_file_header());
        TRY(read_next_image_file_directory());
        return try_make<ExifMetadata>(move(m_metadata));
    }

private:
    enum class ByteOrder {
        LittleEndian,
        BigEndian,
    };

    template<typename T>
    ErrorOr<T> read_value()
    {
        if (m_byte_order == ByteOrder::LittleEndian)
            return TRY(m_stream.read_value<LittleEndian<T>>());
        if (m_byte_order == ByteOrder::BigEndian)
            return TRY(m_stream.read_value<BigEndian<T>>());
        VERIFY_NOT_REACHED();
    }

    ErrorOr<void> set_next_ifd(u32 ifd_offset)
    {
        if (ifd_offset != 0) {
            if (ifd_offset < TRY(m_stream.tell()))
                return Error::from_string_literal("ExifReader: Can not accept an IFD pointing to previous data");

            m_next_ifd = Optional<u32> { ifd_offset };
        } else {
            m_next_ifd = OptionalNone {};
        }
        return {};
    }

    ErrorOr<void> read_next_ifd_offset()
    {
        auto const next_block_position = TRY(read_value<u32>());
        TRY(set_next_ifd(next_block_position));

        return {};
    }

    ErrorOr<void> read_image_file_header()
    {
        // Section 2: TIFF Structure - Image File Header

        auto const byte_order = TRY(m_stream.read_value<u16>());

        switch (byte_order) {
        case 0x4949:
            m_byte_order = ByteOrder::LittleEndian;
            break;
        case 0x4D4D:
            m_byte_order = ByteOrder::BigEndian;
            break;
        default:
            return Error::from_string_literal("ExifReader: Invalid byte order");
        }

        auto const magic_number = TRY(read_value<u16>());

        if (magic_number != 42)
            return Error::from_string_literal("ExifReader: Invalid magic number");

        TRY(read_next_ifd_offset());

        return {};
    }

    ErrorOr<void> read_next_image_file_directory()
    {
        // Section 2: TIFF Structure - Image File Directory

        if (!m_next_ifd.has_value())
            return Error::from_string_literal("ExifReader: Missing an Image File Directory");

        dbgln_if(TIFF_DEBUG, "Reading image file directory at offset {}", m_next_ifd);

        TRY(m_stream.seek(m_next_ifd.value()));

        auto const number_of_field = TRY(read_value<u16>());
        auto next_tag_offset = TRY(m_stream.tell());

        for (u16 i = 0; i < number_of_field; ++i) {
            if (auto maybe_error = read_tag(); maybe_error.is_error() && TIFF_DEBUG)
                dbgln("Unable to decode tag {}/{}", i + 1, number_of_field);

            // Section 2: TIFF Structure
            // IFD Entry
            // Size of tag(u16) + type(u16) + count(u32) + value_or_offset(u32) = 12
            next_tag_offset += 12;
            TRY(m_stream.seek(next_tag_offset));
        }

        TRY(read_next_ifd_offset());
        return {};
    }

    ErrorOr<Vector<Value, 1>> read_tiff_value(Type type, u32 count, u32 offset)
    {
        auto const old_offset = TRY(m_stream.tell());
        ScopeGuard reset_offset { [this, old_offset]() { MUST(m_stream.seek(old_offset)); } };

        TRY(m_stream.seek(offset));

        if (size_of_type(type) * count > m_stream.remaining())
            return Error::from_string_literal("ExifReader: Tag size claims to be bigger that remaining bytes");

        auto const read_every_values = [this, count]<typename T>() -> ErrorOr<Vector<Value>> {
            Vector<Value, 1> result {};
            TRY(result.try_ensure_capacity(count));
            if constexpr (IsSpecializationOf<T, Rational>) {
                for (u32 i = 0; i < count; ++i)
                    result.empend(T { TRY(read_value<typename T::Type>()), TRY(read_value<typename T::Type>()) });
            } else {
                for (u32 i = 0; i < count; ++i)
                    result.empend(typename TypePromoter<T>::Type(TRY(read_value<T>())));
            }
            return result;
        };

        switch (type) {
        case Type::Byte:
        case Type::Undefined: {
            Vector<Value, 1> result;
            auto buffer = TRY(ByteBuffer::create_uninitialized(count));
            TRY(m_stream.read_until_filled(buffer));
            result.append(move(buffer));
            return result;
        }
        case Type::ASCII:
        case Type::UTF8: {
            Vector<Value, 1> result;
            // NOTE: No need to include the null terminator
            if (count > 0)
                --count;
            auto string_data = TRY(ByteBuffer::create_uninitialized(count));
            TRY(m_stream.read_until_filled(string_data));
            result.empend(TRY(String::from_utf8(StringView { string_data.bytes() })));
            return result;
        }
        case Type::UnsignedShort:
            return read_every_values.template operator()<u16>();
        case Type::IFD:
        case Type::UnsignedLong:
            return read_every_values.template operator()<u32>();
        case Type::UnsignedRational:
            return read_every_values.template operator()<Rational<u32>>();
        case Type::SignedLong:
            return read_every_values.template operator()<i32>();
        case Type::SignedRational:
            return read_every_values.template operator()<Rational<i32>>();
        case Type::Float:
            return read_every_values.template operator()<float>();
        case Type::Double:
            return read_every_values.template operator()<double>();
        default:
            VERIFY_NOT_REACHED();
        }
    }

    ErrorOr<void> read_tag()
    {
        auto const tag = TRY(read_value<u16>());
        auto const raw_type = TRY(read_value<u16>());
        auto const type = TRY(tiff_type_from_u16(raw_type));
        auto const count = TRY(read_value<u32>());

        Checked<u32> checked_size = size_of_type(type);
        checked_size *= count;

        if (checked_size.has_overflow())
            return Error::from_string_literal("ExifReader: Invalid tag with too large data");

        auto tiff_value = TRY(([=, this]() -> ErrorOr<Vector<Value>> {
            if (checked_size.value() <= 4) {
                auto value = TRY(read_tiff_value(type, count, TRY(m_stream.tell())));
                TRY(m_stream.discard(4));
                return value;
            }
            auto const offset = TRY(read_value<u32>());
            return read_tiff_value(type, count, offset);
        }()));

        auto subifd_handler = [&](u32 ifd_offset) -> ErrorOr<void> {
            if (auto result = set_next_ifd(ifd_offset); result.is_error()) {
                dbgln("{}", result.error());
                return {};
            }
            TRY(read_next_image_file_directory());
            return {};
        };

        TRY(handle_tag(move(subifd_handler), m_metadata, tag, type, count, move(tiff_value)));

        return {};
    }

    FixedMemoryStream m_stream;
    ByteOrder m_byte_order {};
    Optional<u32> m_next_ifd {};

    ExifMetadata m_metadata {};
};

}

ErrorOr<NonnullOwnPtr<ExifMetadata>> read_exif_metadata(ReadonlyBytes bytes)
{
    ExifReader reader { FixedMemoryStream { bytes } };
    return reader.read_metadata();
}

}
