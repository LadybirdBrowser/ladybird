/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#key-generator-construct
class KeyGenerator {
public:
    [[nodiscard]] u64 current_number() const { return m_current_number; }
    void increment(u64 amount) { m_current_number += amount; }
    void set(u64 value) { m_current_number = value; }

private:
    // A key generator has a current number.
    // The current number is always a positive integer less than or equal to 2^53 (9007199254740992) + 1.
    // The initial value of a key generator's current number is 1, set when the associated object store is created.
    // The current number is incremented as keys are generated, and may be updated to a specific value by using explicit keys.
    u64 m_current_number { 1 };
};

}
