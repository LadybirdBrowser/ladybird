/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashMap;
use std::hash::BuildHasher;
use std::rc::Rc;

pub(crate) trait ShallowCapacityBytes {
    fn shallow_capacity_bytes(&self) -> u64;
}

impl<T: ShallowCapacityBytes + ?Sized> ShallowCapacityBytes for &T {
    fn shallow_capacity_bytes(&self) -> u64 {
        (**self).shallow_capacity_bytes()
    }
}

impl<T> ShallowCapacityBytes for Vec<T> {
    fn shallow_capacity_bytes(&self) -> u64 {
        (self.capacity() * size_of::<T>()) as u64
    }
}

impl<T> ShallowCapacityBytes for Box<[T]> {
    fn shallow_capacity_bytes(&self) -> u64 {
        size_of_val(self.as_ref()) as u64
    }
}

impl<T> ShallowCapacityBytes for Rc<[T]> {
    fn shallow_capacity_bytes(&self) -> u64 {
        size_of_val(self.as_ref()) as u64
    }
}

impl<K, V, S> ShallowCapacityBytes for HashMap<K, V, S>
where
    S: BuildHasher,
{
    fn shallow_capacity_bytes(&self) -> u64 {
        (self.capacity() * (size_of::<K>() + size_of::<V>() + 1)) as u64
    }
}

impl<T, S> ShallowCapacityBytes for std::collections::HashSet<T, S>
where
    S: BuildHasher,
{
    fn shallow_capacity_bytes(&self) -> u64 {
        (self.capacity() * (size_of::<T>() + 1)) as u64
    }
}

macro_rules! capacity_bytes {
    {
        shallow [$($shallow:expr),* $(,)?];
        cached [$($cached:expr),* $(,)?];
        nested [$($nested:expr),* $(,)?];
        skip [$($skip:expr),* $(,)?];
    } => {{
        $(let _ = &$skip;)*
        0_u64
            $(+ $crate::css::style::capacity::ShallowCapacityBytes::shallow_capacity_bytes(&$shallow))*
            $(+ ($cached) as u64)*
            $(+ ($nested) as u64)*
    }};
}

pub(crate) use capacity_bytes;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shallow_hash_table_capacity_includes_control_bytes() {
        let mut table = HashMap::<u32, u64>::new();
        table.reserve(1);

        assert_eq!(
            table.shallow_capacity_bytes(),
            (table.capacity() * (size_of::<u32>() + size_of::<u64>() + 1)) as u64
        );
    }
}
