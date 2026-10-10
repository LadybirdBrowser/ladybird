/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memory nodes read and write, as sets of abstract locations.
//!
//! Every op declares the locations it may read and write beyond its inputs
//! and outputs (see `NodeProperties`). Passes ask one question of them: may
//! a node's writes change what another node read (`Locations::intersects`)?
//! Locations are coarse kinds of memory, so subdividing one (named slots by
//! offset, say) would not change that question.

/// A set of abstract locations.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Locations(u8);

impl Locations {
    pub const NONE: Self = Self(0);
    /// Interpreter frame slots and the fields of execution contexts.
    pub const FRAME: Self = Self(1 << 0);
    /// The values of named properties of objects.
    pub const NAMED_SLOTS: Self = Self(1 << 1);
    /// The shapes of objects, and with them their prototypes and which named
    /// properties they have.
    pub const SHAPES: Self = Self(1 << 2);
    /// The values of indexed elements of objects.
    pub const ELEMENTS: Self = Self(1 << 3);
    /// How objects store their elements: the kind of storage, how many
    /// elements there are and how many there is room for.
    pub const ELEMENT_COUNTS: Self = Self(1 << 4);
    /// The bindings of global declarative environments.
    pub const GLOBAL_BINDINGS: Self = Self(1 << 5);
    /// The bindings of declarative environments at static coordinates. The
    /// nodes that access them also count as accessing `GLOBAL_BINDINGS`,
    /// since a coordinate may lead to the global declarative environment.
    pub const BINDINGS: Self = Self(1 << 6);
    /// Every location.
    pub const ALL: Self = Self((1 << 7) - 1);

    /// Named properties: their values and which objects have them.
    pub const NAMED: Self = Self::NAMED_SLOTS.union(Self::SHAPES);
    /// Indexed elements: their values and how they are stored.
    pub const ELEMENTS_AND_COUNTS: Self = Self::ELEMENTS.union(Self::ELEMENT_COUNTS);

    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }

    pub const fn intersects(self, other: Self) -> bool {
        self.0 & other.0 != 0
    }

    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl std::ops::BitOr for Locations {
    type Output = Self;

    fn bitor(self, other: Self) -> Self {
        self.union(other)
    }
}

impl std::ops::BitOrAssign for Locations {
    fn bitor_assign(&mut self, other: Self) {
        *self = self.union(other);
    }
}

#[cfg(test)]
mod tests {
    use super::Locations;

    #[test]
    fn sets_of_locations() {
        assert!(Locations::NAMED.intersects(Locations::SHAPES));
        assert!(!Locations::NAMED.intersects(Locations::ELEMENTS_AND_COUNTS));
        assert!(!Locations::ELEMENTS.intersects(Locations::ELEMENT_COUNTS));
        assert!(Locations::ALL.intersects(Locations::GLOBAL_BINDINGS));
        assert!(Locations::NONE.is_empty() && !Locations::NONE.intersects(Locations::ALL));
        let mut locations = Locations::FRAME;
        locations |= Locations::ELEMENTS;
        assert_eq!(locations, Locations::FRAME | Locations::ELEMENTS);
    }
}
