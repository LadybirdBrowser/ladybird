/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime's abstract operations and built-in objects, one module per file in Libraries/LibJS/Runtime.

pub mod abstract_operations;
pub mod accessor;
pub mod arguments_object;
pub mod array;
pub mod big_int;
pub mod big_int_algorithms;
pub mod bound_function;
pub mod canonical_index;
pub mod class_construction;
pub mod class_field_definition;
pub mod common_property_names;
pub mod completion;
pub mod declarative_environment;
pub mod descriptor_array;
pub mod ecmascript_function_object;
pub mod environment;
pub mod environment_coordinate;
pub mod environment_shape;
pub mod error;
pub mod error_types;
pub mod function_environment;
pub mod function_object;
pub mod global_environment;
pub mod indexed_properties;
pub mod module_environment;
pub mod native_function;
pub mod number_prototype_algorithms;
pub mod object;
pub mod object_environment;
pub mod primitive_string;
pub mod print;
pub mod private_environment;
pub mod property_attributes;
pub mod property_descriptor;
pub mod property_key;
pub mod realm;
pub mod reference;
pub mod regexp_object;
pub mod shape;
pub mod shared_function_instance_data;
pub mod string_conversions;
pub mod symbol;
pub mod value;
pub mod value_conversions;
