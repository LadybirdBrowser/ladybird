/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::interpreter::vm::Vm;
use crate::runtime::abstract_operations::OptionType;
use crate::runtime::property_key::PropertyKey;

// https://tc39.es/ecma402/#resolution-option-descriptor
pub struct ResolutionOptionDescriptor<'vm> {
    pub key: &'static str,
    pub property: &'vm PropertyKey,
    pub type_: OptionType,
    pub values: &'static [&'static str],
}

impl<'vm> ResolutionOptionDescriptor<'vm> {
    pub fn string(key: &'static str, property: &'vm PropertyKey) -> Self {
        Self {
            key,
            property,
            type_: OptionType::String,
            values: &[],
        }
    }
}

/// The C++ IntlObject, the base of the Intl service objects ResolveOptions resolves the options of: what their
/// constructors' [[RelevantExtensionKeys]] and [[ResolutionOptionDescriptors]] are.
pub trait IntlObject {
    fn relevant_extension_keys(&self) -> &'static [&'static str];
    fn resolution_option_descriptors<'vm>(&self, vm: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>>;
}
