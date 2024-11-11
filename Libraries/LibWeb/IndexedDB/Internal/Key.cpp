/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/Internal/Key.h>
#include <LibWeb/Infra/ByteSequences.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#compare-two-keys
i8 Key::compare_two_keys(Key a, Key b)
{
    // 1. Let ta be the type of a.
    auto ta = a.type();

    // 2. Let tb be the type of b.
    auto tb = b.type();

    // 3. If ta does not equal tb, then run these steps:
    if (ta != tb) {
        // 1. If ta is array, then return 1.
        if (ta == KeyType::Array)
            return 1;

        // 1. If tb is array, then return -1.
        if (tb == KeyType::Array)
            return -1;

        // 1. If ta is binary, then return 1.
        if (ta == KeyType::Binary)
            return 1;

        // 1. If tb is binary, then return -1.
        if (tb == KeyType::Binary)
            return -1;

        // 1. If ta is string, then return 1.
        if (ta == KeyType::String)
            return 1;

        // 1. If tb is string, then return -1.
        if (tb == KeyType::String)
            return -1;

        // 1. If ta is date, then return 1.
        if (ta == KeyType::Date)
            return 1;

        // 1. Assert: tb is date.
        VERIFY(tb == KeyType::Date);

        // 1. Return -1.
        return -1;
    }

    // 4. Let va be the value of a.
    auto va = a.value();

    // 5. Let vb be the value of b.
    auto vb = b.value();

    // 6. Switch on ta:
    switch (ta) {
    // number
    // date
    case KeyType::Number:
    case KeyType::Date: {
        auto a_value = va.get<double>();
        auto b_value = vb.get<double>();

        // 1. If va is greater than vb, then return 1.
        if (a_value > b_value)
            return 1;

        // 2. If va is less than vb, then return -1.
        if (a_value < b_value)
            return -1;

        // 3. Return 0.
        return 0;
    }
    // string
    case KeyType::String: {
        auto a_value = va.get<AK::String>();
        auto b_value = vb.get<AK::String>();

        // 1. If va is code unit less than vb, then return -1.
        if (Infra::code_unit_less_than(a_value, b_value))
            return -1;

        // 2. If vb is code unit less than va, then return 1.
        if (Infra::code_unit_less_than(b_value, a_value))
            return 1;

        // 3. Return 0.
        return 0;
    }
    // binary
    case KeyType::Binary: {
        auto a_value = va.get<ByteBuffer>();
        auto b_value = vb.get<ByteBuffer>();

        // 1. If va is byte less than vb, then return -1.
        if (Infra::is_byte_less_than(a_value, b_value))
            return -1;

        // 2. If vb is byte less than va, then return 1.
        if (Infra::is_byte_less_than(b_value, a_value))
            return 1;

        // 3. Return 0.
        return 0;
    }
    // array
    case KeyType::Array: {
        auto a_value = va.get<Vector<Key>>();
        auto b_value = vb.get<Vector<Key>>();

        // 1. Let length be the lesser of va’s size and vb’s size.
        auto length = min(a_value.size(), b_value.size());

        // 2. Let i be 0.
        u64 i = 0;

        // 3. While i is less than length, then:
        while (i < length) {
            // 1. Let c be the result of recursively comparing two keys with va[i] and vb[i].
            auto c = compare_two_keys(a_value[i], b_value[i]);

            // 2. If c is not 0, return c.
            if (c != 0)
                return c;

            // 3. Increase i by 1.
            i++;
        }

        // 4. If va’s size is greater than vb’s size, then return 1.
        if (a_value.size() > b_value.size())
            return 1;

        // 5. If va’s size is less than vb’s size, then return -1.
        if (a_value.size() < b_value.size())
            return -1;

        // 6. Return 0.
        return 0;
    }
    }

    VERIFY_NOT_REACHED();
}

}
