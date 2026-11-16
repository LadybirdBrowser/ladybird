/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Prints the events of the documents named on the command line.

fn main() {
    let options = libxml_rust::Options {
        preserve_comments: true,
        preserve_cdata: true,
        resolve_html_entity: None,
    };
    for path in std::env::args().skip(1) {
        let source = std::fs::read(&path).unwrap();
        let document = libxml_rust::parse(&source, options);
        print!("{}", libxml_rust::dump::dump(&document, &source, true));
    }
}
