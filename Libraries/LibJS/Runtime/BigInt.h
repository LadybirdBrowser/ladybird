/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>

namespace JS {

class JS_API BigInt final : public EngineCell {
public:
    [[nodiscard]] static GC::Ref<BigInt> create(VM&, Crypto::SignedBigInteger);

    // https://tc39.es/ecma262/#sec-stringtobigint
    [[nodiscard]] static GC::Ptr<BigInt> create_from_string(VM&, Utf16View const&);

    // The integer lives in the Rust runtime's cell, so this returns a copy rather than a reference to it.
    Crypto::SignedBigInteger big_integer() const;

    // The decimal digits, after a "-" if it is negative, which is ToString(bigint).
    String to_string() const;

    Utf16String to_utf16_string() const;
};

}
