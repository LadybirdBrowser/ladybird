/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A rustc driver that adds Ladybird's GC lints. Use it as cargo's `RUSTC_WORKSPACE_WRAPPER`, so it compiles the
//! workspace members and leaves dependencies to plain rustc, or run it in place of rustc on a single file.

#![feature(rustc_private)]

extern crate rustc_data_structures;
extern crate rustc_driver;
extern crate rustc_errors;
extern crate rustc_hir;
extern crate rustc_interface;
extern crate rustc_lint;
extern crate rustc_middle;
extern crate rustc_session;
extern crate rustc_span;

mod gc_lints;

use std::path::Path;
use std::process::ExitCode;

struct Callbacks;

impl rustc_driver::Callbacks for Callbacks {
    fn config(&mut self, config: &mut rustc_interface::Config) {
        config.register_lints = Some(Box::new(|_, lint_store| {
            lint_store.register_lints(gc_lints::LINTS);
            lint_store.register_late_pass(|_| Box::new(gc_lints::GcLints::default()));
        }));
    }
}

fn main() -> ExitCode {
    let mut args: Vec<String> = std::env::args().collect();

    // As a wrapper, cargo passes the path of the real rustc first.
    if args
        .get(1)
        .is_some_and(|arg| Path::new(arg).file_stem().is_some_and(|stem| stem == "rustc"))
    {
        args.remove(1);
    }

    // Code that names the lints in an attribute does so under this cfg, which only the driver sets:
    // #[cfg_attr(ladybird_gc_lints, expect(gc_container_on_stack, reason = "..."))]
    let is_query = args
        .iter()
        .any(|arg| arg == "-vV" || arg == "-V" || arg.starts_with("--print"));
    if !is_query {
        args.extend(
            ["--cfg", "ladybird_gc_lints", "--check-cfg", "cfg(ladybird_gc_lints)"]
                .into_iter()
                .map(String::from),
        );
        // For surveying a tree with hits: a deny would stop the crates that depend on the first crate with one.
        if std::env::var_os("LADYBIRD_RUST_LINTS_WARN").is_some() {
            for lint in gc_lints::LINTS {
                args.extend(["--warn".to_owned(), lint.name_lower()]);
            }
        }
    }

    rustc_driver::install_ice_hook("https://github.com/LadybirdBrowser/ladybird/issues", |_| ());
    rustc_driver::catch_with_exit_code(|| rustc_driver::run_compiler(&args, &mut Callbacks))
}
