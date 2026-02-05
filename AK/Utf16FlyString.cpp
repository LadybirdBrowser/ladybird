/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Singleton.h>
#include <AK/Utf16FlyString.h>

namespace AK {

struct Utf16FlyStringTableHashTraits : public Traits<Detail::Utf16StringData const*> {
    static u32 hash(Detail::Utf16StringData const* string) { return string->hash(); }
    static bool equals(Detail::Utf16StringData const* a, Detail::Utf16StringData const* b) { return *a == *b; }
    static constexpr bool may_have_slow_equality_check() { return true; }
};

static auto& all_utf16_fly_strings()
{
    static Singleton<HashTable<Detail::Utf16StringData const*, Utf16FlyStringTableHashTraits>> table;
    return *table;
}

namespace Detail {

void did_destroy_utf16_fly_string_data(Badge<Detail::Utf16StringData>, Detail::Utf16StringData const& data)
{
    all_utf16_fly_strings().remove(&data);
}

}

template<typename ViewType>
Optional<Utf16FlyString> Utf16FlyString::create_fly_string_from_cache(ViewType const& string)
{
    if (string.is_empty())
        return {};

    if constexpr (IsSame<ViewType, StringView>) {
        if (string.length() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && string.is_ascii())
            return Utf16String::from_utf8_without_validation(string);
    } else {
        if (string.length_in_code_units() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && string.is_ascii())
            return Utf16String::from_utf16(string);
    }

    if (auto it = all_utf16_fly_strings().find(string.hash(), [&](auto const& entry) { return *entry == string; }); it != all_utf16_fly_strings().end())
        return Utf16FlyString { Detail::Utf16StringBase(**it) };

    return {};
}

Utf16FlyString Utf16FlyString::from_utf8(StringView string)
{
    if (auto result = create_fly_string_from_cache(string); result.has_value())
        return result.release_value();
    return Utf16String::from_utf8(string);
}

Utf16FlyString Utf16FlyString::from_utf8_without_validation(StringView string)
{
    if (auto result = create_fly_string_from_cache(string); result.has_value())
        return result.release_value();
    return Utf16String::from_utf8_without_validation(string);
}

Utf16FlyString Utf16FlyString::from_utf16(Utf16View const& string)
{
    if (auto result = create_fly_string_from_cache(string); result.has_value())
        return result.release_value();
    return Utf16String::from_utf16(string);
}

Utf16FlyString::Utf16FlyString(Utf16String const& string)
{
    if (string.has_short_ascii_storage()) {
        m_data = string;
        return;
    }

    auto const* data = string.data({});

    if (data->is_fly_string()) {
        m_data = string;
        return;
    }

    if (auto it = all_utf16_fly_strings().find(data); it == all_utf16_fly_strings().end()) {
        m_data = string;

        all_utf16_fly_strings().set(data);
        data->mark_as_fly_string({});
    } else {
        m_data.set_data({}, *it);
    }
}

size_t Utf16FlyString::number_of_utf16_fly_strings()
{
    return all_utf16_fly_strings().size();
}

}
