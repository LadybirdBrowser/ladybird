/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Temporal abstract operations and built-in objects.

pub mod abstract_operations;
pub mod calendar;
pub mod date_equations;
pub mod duration;
pub mod duration_constructor;
pub mod duration_prototype;
pub mod instant;
pub mod instant_constructor;
pub mod instant_prototype;
pub mod iso8601;
pub mod iso_records;
pub mod now;
pub mod plain_date;
pub mod plain_date_constructor;
pub mod plain_date_prototype;
pub mod plain_date_time;
pub mod plain_date_time_constructor;
pub mod plain_date_time_prototype;
pub mod plain_month_day;
pub mod plain_month_day_constructor;
pub mod plain_month_day_prototype;
pub mod plain_time;
pub mod plain_time_constructor;
pub mod plain_time_prototype;
pub mod plain_year_month;
pub mod plain_year_month_constructor;
pub mod plain_year_month_prototype;
#[allow(
    clippy::module_inception,
    reason = "the module defines the Temporal object of the Temporal namespace"
)]
pub mod temporal;
pub mod time_zone;
pub mod zoned_date_time;
pub mod zoned_date_time_constructor;
pub mod zoned_date_time_prototype;
