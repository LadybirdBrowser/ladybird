/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceServerTiming, https://w3c.github.io/server-timing/#the-performanceservertiming-interface

use core::ffi::c_void;
use std::collections::HashMap;

use ak::{GenericLexer, Utf16String};
use libhttp_rust::http::{
    collect_an_http_quoted_string_extracting_the_value, is_http_tab_or_space, is_http_token_code_point,
};
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::numbers::parse_floating_point_number;

fn consume_token<'a>(lexer: &mut GenericLexer<'a, u8>) -> &'a [u8] {
    let token = lexer.consume_while(is_http_token_code_point);
    lexer.ignore_while(is_http_tab_or_space);
    token
}

// https://w3c.github.io/server-timing/#dfn-parse-a-server-timing-header-field
// INTEROP: The parsing algorithm in the spec accepts metric names and parameter values that are not tokens, and matches
//          parameter names case-sensitively. Other engines and WPT instead follow the ABNF of the header field, which is
//          what we do here:
//
//          Server-Timing             = #server-timing-metric
//          server-timing-metric      = metric-name *( OWS ";" OWS server-timing-param )
//          metric-name               = token
//          server-timing-param       = server-timing-param-name OWS "=" OWS server-timing-param-value
//          server-timing-param-name  = token
//          server-timing-param-value = token / quoted-string
//
//          Parameter names are matched ASCII case-insensitively, only the first instance of each parameter is
//          considered, and extraneous characters after a metric name or a parameter value are ignored.
fn parse_a_server_timing_header_field(field: &[u8]) -> Option<PerformanceServerTiming> {
    let mut lexer = GenericLexer::new(field);

    lexer.ignore_while(is_http_tab_or_space);

    let name = consume_token(&mut lexer);
    if name.is_empty() {
        return None;
    }

    let mut metric = PerformanceServerTiming {
        metric_name: String::from_utf8(name.to_vec()).expect("a token is ASCII"),
        params: HashMap::new(),
    };

    lexer.ignore_until(b';');

    while lexer.consume_specific(b';') {
        lexer.ignore_while(is_http_tab_or_space);

        let param_name = consume_token(&mut lexer);
        if param_name.is_empty() {
            break;
        }

        let mut param_value = String::new();
        if lexer.consume_specific(b'=') {
            lexer.ignore_while(is_http_tab_or_space);

            if lexer.peek() == Some(b'"') {
                let quoted_string = collect_an_http_quoted_string_extracting_the_value(&mut lexer);
                if quoted_string.closed {
                    // NB: A quoted-string can contain obs-text, which is not necessarily valid UTF-8.
                    param_value = String::from_utf8_lossy(&quoted_string.value).into_owned();
                }
            } else {
                param_value = String::from_utf8(consume_token(&mut lexer).to_vec()).expect("a token is ASCII");
            }

            lexer.ignore_until(b';');
        }

        let lowercase_param_name = String::from_utf8(param_name.to_ascii_lowercase()).expect("a token is ASCII");
        metric.params.entry(lowercase_param_name).or_insert(param_value);
    }

    Some(metric)
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceServerTiming {
    // https://w3c.github.io/server-timing/#dfn-metric-name
    metric_name: String,

    // https://w3c.github.io/server-timing/#dfn-params
    params: HashMap<String, String>,
}

impl Wrappable for PerformanceServerTiming {}

impl PerformanceServerTiming {
    // https://w3c.github.io/server-timing/#dom-performanceservertiming-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(Utf16String::from_utf8(&self.metric_name))
    }

    // https://w3c.github.io/server-timing/#dom-performanceservertiming-duration
    pub(crate) fn duration(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // 1. If this’s params["dur"] does not exist, return 0.
        let Some(dur_param) = self.params.get("dur") else {
            return Ok(0.0);
        };

        // 2. Let dur be the result of parsing this’s params["dur"] using the rules for parsing floating-point number
        //    values.
        let dur = parse_floating_point_number(dur_param);

        // 3. If dur is an error, return 0; Otherwise return dur.
        Ok(dur.unwrap_or(0.0))
    }

    // https://w3c.github.io/server-timing/#dom-performanceservertiming-description
    pub(crate) fn description(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The description getter steps are to return this’s params["desc"] if it exists, otherwise the empty string.
        Ok(Utf16String::from_utf8(
            self.params.get("desc").map_or("", String::as_str),
        ))
    }
}

/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_performance_server_timing_parse_a_server_timing_header_field(
    vm: *mut c_void,
    field: ak::Span<'_, u8>,
) -> *mut c_void {
    let Some(metric) = parse_a_server_timing_header_field(field.as_slice()) else {
        return core::ptr::null_mut();
    };
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    cell_into_abi(allocate(vm, |_| metric))
}
