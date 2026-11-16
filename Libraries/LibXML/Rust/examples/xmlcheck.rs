/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Parses each document whose path is a line of standard input and prints the path with "ok", or with the first
//! error and whether it is fatal. Every result is also put through the verifier.

use std::io::BufRead;
use std::io::Write;

fn main() {
    let options = libxml_rust::Options {
        preserve_comments: true,
        preserve_cdata: true,
        resolve_html_entity: None,
    };
    let stdout = std::io::stdout();
    let mut out = std::io::BufWriter::new(stdout.lock());
    for path in std::io::stdin().lock().lines() {
        let path = path.unwrap();
        let Ok(source) = std::fs::read(&path) else {
            writeln!(out, "{path}\tunreadable").unwrap();
            continue;
        };
        let document = libxml_rust::parse(&source, options);
        if let Err(problem) = libxml_rust::verify::verify(&document, &source) {
            writeln!(out, "{path}\tinvalid\t{problem}").unwrap();
            continue;
        }
        match document.errors.first() {
            None => writeln!(out, "{path}\tok").unwrap(),
            Some(error) => {
                let severity = if error.fatal { "fatal" } else { "error" };
                writeln!(
                    out,
                    "{path}\t{severity}\t{}:{}: {}",
                    error.line, error.column, error.message
                )
                .unwrap();
            }
        }
    }
}
