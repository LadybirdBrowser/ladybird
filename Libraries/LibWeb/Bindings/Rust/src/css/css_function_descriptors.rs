/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSFunctionDescriptors, https://drafts.csswg.org/css-mixins-1/#cssfunctiondescriptors

use super::css_descriptors::descriptors_interface;

descriptors_interface!(
    CSSFunctionDescriptors,
    web_css_function_descriptors_create,
    (result, set_result, "result"),
);
