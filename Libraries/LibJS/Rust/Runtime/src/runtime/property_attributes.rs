/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;

/// Mirrors JS::Attribute: the bits of PropertyAttributes.
pub struct Attribute;

impl Attribute {
    pub const WRITABLE: u8 = 1 << 0;
    pub const ENUMERABLE: u8 = 1 << 1;
    pub const CONFIGURABLE: u8 = 1 << 2;
}

// 6.1.7.1 Property Attributes, https://tc39.es/ecma262/#sec-property-attributes
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
pub struct PropertyAttributes {
    bits: u8,
}

impl PropertyAttributes {
    pub const fn new(bits: u8) -> Self {
        Self { bits }
    }

    pub const fn is_writable(self) -> bool {
        self.bits & Attribute::WRITABLE != 0
    }

    pub const fn is_enumerable(self) -> bool {
        self.bits & Attribute::ENUMERABLE != 0
    }

    pub const fn is_configurable(self) -> bool {
        self.bits & Attribute::CONFIGURABLE != 0
    }

    pub const fn set_writable(&mut self, writable: bool) {
        self.set_bit(Attribute::WRITABLE, writable);
    }

    pub const fn set_enumerable(&mut self, enumerable: bool) {
        self.set_bit(Attribute::ENUMERABLE, enumerable);
    }

    pub const fn set_configurable(&mut self, configurable: bool) {
        self.set_bit(Attribute::CONFIGURABLE, configurable);
    }

    pub const fn bits(self) -> u8 {
        self.bits
    }

    const fn set_bit(&mut self, bit: u8, value: bool) {
        if value {
            self.bits |= bit;
        } else {
            self.bits &= !bit;
        }
    }
}

impl From<u8> for PropertyAttributes {
    fn from(bits: u8) -> Self {
        Self::new(bits)
    }
}

pub const DEFAULT_ATTRIBUTES: PropertyAttributes =
    PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE | Attribute::ENUMERABLE);

impl fmt::Debug for PropertyAttributes {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "PropertyAttributes {{ [[Writable]]: {}, [[Enumerable]]: {}, [[Configurable]]: {} }}",
            self.is_writable(),
            self.is_enumerable(),
            self.is_configurable()
        )
    }
}
