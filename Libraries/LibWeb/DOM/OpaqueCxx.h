/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::DOM {

// A C++ member inside a prefix that libweb_rust lays out (see Rust/src/dom/prefix.rs), as `count` units of T, which give
// it the member's size and alignment. C++ never makes one: it only names the prefixes to assert that its classes start
// with them, and keeps the member itself where the slot is.
template<typename T, size_t count>
struct OpaqueCxx {
    T units[count];
};

}
