/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>

namespace JS {

class BigInt final : public Cell {
    GC_CELL(BigInt, Cell);
    GC_DECLARE_ALLOCATOR(BigInt);

public:
    [[nodiscard]] static GC::Ref<BigInt> create(VM&, Crypto::SignedBigInteger);

    virtual ~BigInt() override = default;

    Crypto::SignedBigInteger const& big_integer() const { return m_big_integer; }

    ErrorOr<String> to_string() const;
    ByteString to_byte_string() const { return ByteString::formatted("{}n", m_big_integer.to_base_deprecated(10)); }

private:
    explicit BigInt(Crypto::SignedBigInteger);

    Crypto::SignedBigInteger m_big_integer;
};

ThrowCompletionOr<BigInt*> number_to_bigint(VM&, Value);

}
