/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// The typedef VibratePattern of NotificationsAPI/Notification.idl, whose union (unsigned long or
/// sequence<unsigned long>) other interfaces also name.
pub(crate) enum VibratePattern {
    UnsignedLong(u32),
    Sequence(Vec<u32>),
}
