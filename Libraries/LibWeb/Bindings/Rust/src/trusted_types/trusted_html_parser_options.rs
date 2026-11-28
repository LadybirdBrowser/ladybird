/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TrustedHTMLParserOptions, https://w3c.github.io/trusted-types/dist/spec/#trusted-html-parser-options

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::interface::{InterfaceCell, Ref, Wrappable, allocate};
use crate::html::sanitizer_configuration::SanitizerConfiguration;

/// https://w3c.github.io/trusted-types/dist/spec/#trusted-html-parser-options
#[derive(Trace)]
pub(crate) struct TrustedHTMLParserOptions {
    realm: Gc<Realm>,
    // TrustedHTMLParserOptions objects have an associated boolean runScripts and an associated SanitizerConfig-or-null
    // sanitizer configuration. These values are set when the object is created, and will never change during its
    // lifetime.
    #[gc(untraced)]
    sanitizer_configuration: Option<SanitizerConfiguration>,
    run_scripts: bool,
}

impl Wrappable for TrustedHTMLParserOptions {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

impl TrustedHTMLParserOptions {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        sanitizer_configuration: Option<SanitizerConfiguration>,
        run_scripts: bool,
    ) -> Ref<Self> {
        allocate(vm, |_| Self {
            realm,
            sanitizer_configuration,
            run_scripts,
        })
    }

    pub(crate) fn sanitizer_configuration(&self) -> Option<&SanitizerConfiguration> {
        self.sanitizer_configuration.as_ref()
    }

    pub(crate) fn run_scripts(&self) -> bool {
        self.run_scripts
    }
}

/// The runScripts of the TrustedHTMLParserOptions.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_trusted_types_trusted_html_parser_options_run_scripts(cell: &InterfaceCell) -> bool {
    cell.object::<TrustedHTMLParserOptions>().run_scripts()
}
