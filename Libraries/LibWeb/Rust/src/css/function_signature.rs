/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::syntax::SyntaxNode;
use crate::css::style_value::StyleValueData;
use std::ffi::c_void;
use std::sync::Arc;

pub(crate) struct FunctionParameterData {
    pub(crate) name: CssString,
    pub(crate) syntax: Arc<SyntaxNode>,
    pub(crate) default_value: Option<Arc<StyleValueData>>,
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

#[repr(C)]
pub struct FfiFunctionSignatureView {
    pub name: FfiUtf16View,
    pub parameter_count: usize,
    pub return_type: *const c_void,
}

#[repr(C)]
pub struct FfiFunctionParameterView {
    pub name: FfiUtf16View,
    pub syntax: *const c_void,
    pub default_value: *const c_void,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_function_signature_view(signature: &FunctionSignature) -> FfiFunctionSignatureView {
    let name = signature.name.units();
    FfiFunctionSignatureView {
        name: FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: name.as_ptr(),
            length: name.len(),
        },
        parameter_count: signature.parameters.len(),
        return_type: Arc::as_ptr(&signature.return_type).cast(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_function_signature_parameter(
    signature: &FunctionSignature,
    index: usize,
) -> FfiFunctionParameterView {
    let parameter = &signature.parameters[index];
    let name = parameter.name.units();
    FfiFunctionParameterView {
        name: FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: name.as_ptr(),
            length: name.len(),
        },
        syntax: Arc::as_ptr(&parameter.syntax).cast(),
        default_value: parameter
            .default_value
            .as_ref()
            .map_or(std::ptr::null(), |value| Arc::as_ptr(value).cast()),
    }
}
