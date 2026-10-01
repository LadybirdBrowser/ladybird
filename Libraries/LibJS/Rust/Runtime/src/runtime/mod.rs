/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime's abstract operations and built-in objects, one module per file in Libraries/LibJS/Runtime.

pub mod abstract_operations;
pub mod accessor;
pub mod array;
pub mod big_int;
pub mod big_int_algorithms;
pub mod common_property_names;
pub mod completion;
pub mod descriptor_array;
pub mod error;
pub mod error_types;
pub mod indexed_properties;
pub mod number_prototype_algorithms;
pub mod object;
pub mod primitive_string;
pub mod print;
pub mod private_environment;
pub mod property_attributes;
pub mod property_descriptor;
pub mod property_key;
pub mod realm;
pub mod regexp_object;
pub mod shape;
pub mod string_conversions;
pub mod symbol;
pub mod value;
pub mod value_conversions;
