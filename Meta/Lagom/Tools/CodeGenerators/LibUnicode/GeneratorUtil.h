/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <AK/SourceGenerator.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/File.h>

template<typename StorageType>
class UniqueStorage {
public:
    size_t ensure(StorageType value)
    {
        // We maintain a set of unique values in two structures: a vector which stores the values in
        // the order they are added, and a hash map which maps that value to its index in the vector.
        // The vector is to ensure the values are generated in an easily known order, and the map is
        // to allow quickly deciding if a value is actually unique (otherwise, we'd have to linearly
        // search the vector for each value).
        //
        // Also note that index 0 will be reserved for the default-initialized value, so the index
        // returned from this method is actually the real index in the vector + 1.
        if (auto index = m_storage_indices.get(value); index.has_value())
            return *index;

        m_storage.append(move(value));

        auto storage_index = m_storage.size();
        m_storage_indices.set(m_storage.last(), storage_index);

        return storage_index;
    }

    StringView type_that_fits() const
    {
        if (m_storage.size() <= NumericLimits<u8>::max())
            return "u8"sv;
        if (m_storage.size() <= NumericLimits<u16>::max())
            return "u16"sv;
        if (m_storage.size() <= NumericLimits<u32>::max())
            return "u32"sv;
        return "u64"sv;
    }

protected:
    Vector<StorageType> m_storage;
    HashMap<StorageType, size_t> m_storage_indices;
};

class UniqueStringStorage : public UniqueStorage<ByteString> {
    using Base = UniqueStorage<ByteString>;

public:
    // The goal of the string table generator is to ensure the table is located within the read-only
    // section of the shared library. If StringViews are generated directly, the table will be located
    // in the initialized data section. So instead, we generate run-length encoded (RLE) arrays to
    // represent the strings.
    void generate(SourceGenerator& generator) const
    {
        constexpr size_t max_values_per_row = 300;
        size_t values_in_current_row = 0;

        auto append_hex_value = [&](auto value) {
            if (values_in_current_row++ > 0)
                generator.append(", ");

            generator.append(ByteString::formatted("{:#x}", value));

            if (values_in_current_row == max_values_per_row) {
                values_in_current_row = 0;
                generator.append(",\n    ");
            }
        };

        Vector<u32> string_indices;
        string_indices.ensure_capacity(Base::m_storage.size());
        u32 next_index { 0 };

        for (auto const& string : Base::m_storage) {
            // Ensure the string length may be encoded as two u8s.
            VERIFY(string.length() <= NumericLimits<u16>::max());

            string_indices.unchecked_append(next_index);
            next_index += string.length() + 2;
        }

        generator.set("size", ByteString::number(next_index));
        generator.append(R"~~~(
static constexpr Array<u8, @size@> s_encoded_strings { {
    )~~~");

        for (auto const& string : Base::m_storage) {
            auto length = string.length();
            append_hex_value((length & 0xff00) >> 8);
            append_hex_value(length & 0x00ff);

            for (auto ch : string)
                append_hex_value(static_cast<u8>(ch));
        }

        generator.append(R"~~~(
} };
)~~~");

        generator.set("size", ByteString::number(string_indices.size()));
        generator.append(R"~~~(
static constexpr Array<u32, @size@> s_encoded_string_indices { {
    )~~~");

        values_in_current_row = 0;
        for (auto index : string_indices)
            append_hex_value(index);

        generator.append(R"~~~(
} };

static constexpr StringView decode_string(size_t index)
{
    if (index == 0)
        return {};

    index = s_encoded_string_indices[index - 1];

    auto length_high = s_encoded_strings[index];
    auto length_low = s_encoded_strings[index + 1];

    size_t length = (length_high << 8) | length_low;
    if (length == 0)
        return {};

    auto const* start = &s_encoded_strings[index + 2];
    return { reinterpret_cast<char const*>(start), length };
}
)~~~");
    }
};

inline ErrorOr<NonnullOwnPtr<Core::InputBufferedFile>> open_file(StringView path, Core::File::OpenMode mode)
{
    if (path.is_empty())
        return Error::from_string_literal("Provided path is empty, please provide all command line options");

    auto file = TRY(Core::File::open(path, mode));
    return Core::InputBufferedFile::create(move(file));
}
