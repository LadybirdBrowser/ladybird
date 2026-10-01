/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

macro_rules! define_class_ids {
    ($($name:ident,)*) => {
        /// Every class of cell the runtime allocates, which indexes each heap's table of allocators.
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(u16)]
        pub enum ClassId {
            $($name,)*
        }

        pub const CLASS_COUNT: usize = [$(ClassId::$name,)*].len();
    };
}

define_class_ids! {
    Executable,
}
