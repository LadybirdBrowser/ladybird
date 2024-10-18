/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Wasm::Constants {

// Value
inline constexpr auto i32_tag = 0x7f;
inline constexpr auto i64_tag = 0x7e;
inline constexpr auto f32_tag = 0x7d;
inline constexpr auto f64_tag = 0x7c;
inline constexpr auto v128_tag = 0x7b;
inline constexpr auto function_reference_tag = 0x70;
inline constexpr auto extern_reference_tag = 0x6f;

// Function
inline constexpr auto function_signature_tag = 0x60;

// Global
inline constexpr auto const_tag = 0x00;
inline constexpr auto var_tag = 0x01;

// Block
inline constexpr auto empty_block_tag = 0x40;

// Import section
inline constexpr auto extern_function_tag = 0x00;
inline constexpr auto extern_table_tag = 0x01;
inline constexpr auto extern_memory_tag = 0x02;
inline constexpr auto extern_global_tag = 0x03;

inline constexpr auto page_size = 64 * KiB;

// Implementation-defined limits
// These are not concretely defined by the spec, so the values are only defined by us.
inline constexpr auto minimum_stack_space_to_keep_free = 256 * KiB; // Note: Value is arbitrary and chosen by testing with ASAN
inline constexpr auto max_allowed_executed_instructions_per_call = 256 * 1024 * 1024;
inline constexpr auto max_allowed_vector_size = 500 * MiB;
inline constexpr auto max_allowed_function_locals_per_type = 42069; // Note: VERY arbitrary.

}
