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
#include <AK/Vector.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>

namespace JS {

class JS_API BigInt final : public EngineCell {
public:
    // The magnitude is in 32-bit words, least significant first.
    [[nodiscard]] static GC::Ref<BigInt> create(VM&, bool is_negative, ReadonlySpan<u32> magnitude_words);
    [[nodiscard]] static GC::Ref<BigInt> create_from_i64(VM&, i64);
    [[nodiscard]] static GC::Ref<BigInt> create_from_u64(VM&, u64);

    // https://tc39.es/ecma262/#sec-stringtobigint
    [[nodiscard]] static GC::Ptr<BigInt> create_from_string(VM&, Utf16View const&);

    bool is_negative() const;
    // The magnitude in 32-bit words, least significant first, without leading zero words.
    Vector<u32> magnitude_words() const;

    // The decimal digits, after a "-" if it is negative, which is ToString(bigint).
    String to_string() const;

    Utf16String to_utf16_string() const;
};

}
