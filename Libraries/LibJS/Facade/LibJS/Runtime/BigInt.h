/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/EngineCell.h>

namespace JS {

class JS_API BigInt final : public EngineCell {
public:
    [[nodiscard]] static GC::Ref<BigInt> create(VM&, Crypto::SignedBigInteger);

    // The integer lives in the Rust runtime's cell, so this returns a copy rather than a reference to it.
    Crypto::SignedBigInteger big_integer() const;

    Utf16String to_utf16_string() const;
};

}
