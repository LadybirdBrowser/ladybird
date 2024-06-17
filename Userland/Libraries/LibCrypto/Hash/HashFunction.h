/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace Crypto::Hash {

template<size_t DigestS>
struct Digest {
    static_assert(DigestS % 8 == 0);
    constexpr static size_t Size = DigestS / 8;
    u8 data[Size];

    [[nodiscard]] ALWAYS_INLINE u8 const* immutable_data() const { return data; }
    [[nodiscard]] ALWAYS_INLINE size_t data_length() const { return Size; }

    [[nodiscard]] ALWAYS_INLINE ReadonlyBytes bytes() const { return { immutable_data(), data_length() }; }

    [[nodiscard]] bool operator==(Digest const& other) const = default;
    [[nodiscard]] bool operator!=(Digest const& other) const = default;
};

template<size_t BlockS, size_t DigestS, typename DigestT = Digest<DigestS>>
class HashFunction {
public:
    static_assert(BlockS % 8 == 0);
    static constexpr auto BlockSize = BlockS / 8;

    static_assert(DigestS % 8 == 0);
    static constexpr auto DigestSize = DigestS / 8;

    using DigestType = DigestT;

    constexpr static size_t block_size() { return BlockSize; }
    constexpr static size_t digest_size() { return DigestSize; }

    virtual void update(u8 const*, size_t) = 0;

    void update(Bytes buffer) { update(buffer.data(), buffer.size()); }
    void update(ReadonlyBytes buffer) { update(buffer.data(), buffer.size()); }
    void update(ByteBuffer const& buffer) { update(buffer.data(), buffer.size()); }
    void update(StringView string) { update((u8 const*)string.characters_without_null_termination(), string.length()); }

    virtual DigestType peek() = 0;
    virtual DigestType digest() = 0;

    virtual void reset() = 0;

    virtual ByteString class_name() const = 0;

protected:
    virtual ~HashFunction() = default;
};
}

template<size_t DigestS>
struct AK::Formatter<Crypto::Hash::Digest<DigestS>> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder& builder, Crypto::Hash::Digest<DigestS> const& digest)
    {
        for (size_t i = 0; i < digest.Size; ++i) {
            if (i > 0 && i % 4 == 0)
                TRY(builder.put_padding('-', 1));
            TRY(builder.put_u64(digest.data[i], 16, false, false, true, false, FormatBuilder::Align::Right, 2));
        }
        return {};
    }
};
