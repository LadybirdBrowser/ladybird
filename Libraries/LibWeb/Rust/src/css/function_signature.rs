/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::parser::syntax::SyntaxNode;
use crate::css::style_value_arc::StyleValueArc;
use std::sync::Arc;

pub(crate) struct FunctionParameterData {
    pub(crate) name: ak::Utf16FlyString,
    pub(crate) syntax: Arc<SyntaxNode>,
    pub(crate) default_value: Option<StyleValueArc>,
}

pub struct FunctionSignature {
    pub(crate) name: CssString,
    pub(crate) parameters: Box<[FunctionParameterData]>,
    pub(crate) return_type: Arc<SyntaxNode>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<FunctionSignature>();
};
