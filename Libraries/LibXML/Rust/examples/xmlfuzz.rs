/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Parses the files named on the command line (directories are walked) and then mutations of them, and checks
//! every resulting document with `verify`. A document that fails the check, or a panic, is a bug in the parser.
//!
//! usage: xmlfuzz [--mutations N] [--max-size BYTES] PATH...

use std::path::Path;

use libxml_rust::Options;
use libxml_rust::verify::verify;

struct Random(u64);

impl Random {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn below(&mut self, bound: usize) -> usize {
        if bound == 0 {
            0
        } else {
            (self.next() % bound as u64) as usize
        }
    }
}

const FRAGMENTS: &[&[u8]] = &[
    b"<",
    b">",
    b"/>",
    b"</",
    b"&",
    b";",
    b"\"",
    b"'",
    b"=",
    b"]]>",
    b"<!--",
    b"-->",
    b"--",
    b"<![CDATA[",
    b"<?",
    b"?>",
    b"&#x",
    b"&#",
    b"&amp;",
    b"&lt;",
    b"&e;",
    b"%e;",
    b"xmlns",
    b"xmlns:",
    b"xml:",
    b":",
    b"<!DOCTYPE a [",
    b"]>",
    b"<!ENTITY e \"",
    b"<!ENTITY % e \"",
    b"<!ATTLIST a b CDATA \"",
    b"<!ELEMENT a ANY>",
    b"<?xml version=\"1.0\"?>",
    b"\r",
    b"\r\n",
    b"\t",
    b"\0",
    b"\x0C",
    b"\x80",
    b"\xC3",
    b"\xC3\xA5",
    b"\xE4\xB8\xAD",
    b"\xED\xA0\x80",
    b"\xEF\xBF\xBE",
    b"\xEF\xBF\xBF",
    b"\xF0\x9F\x98\x80",
    b"\xF4\x90\x80\x80",
    b"\xFF",
    b"\xEF\xBB\xBF",
    b" ",
    b"a",
    b"<a>",
    b"</a>",
];

fn mutate(random: &mut Random, source: &[u8]) -> Vec<u8> {
    let mut bytes = source.to_vec();
    for _ in 0..1 + random.below(4) {
        let at = random.below(bytes.len() + 1);
        match random.below(7) {
            0 if !bytes.is_empty() => {
                let at = at.min(bytes.len() - 1);
                bytes[at] ^= 1 << random.below(8);
            }
            1 if !bytes.is_empty() => {
                let at = at.min(bytes.len() - 1);
                bytes[at] = random.next() as u8;
            }
            2 => {
                let fragment = FRAGMENTS[random.below(FRAGMENTS.len())];
                bytes.splice(at..at, fragment.iter().copied());
            }
            3 => {
                let end = (at + random.below(64)).min(bytes.len());
                bytes.drain(at..end);
            }
            4 => {
                let end = (at + random.below(256)).min(bytes.len());
                let copy = bytes[at..end].to_vec();
                let to = random.below(bytes.len() + 1);
                bytes.splice(to..to, copy);
            }
            5 => bytes.truncate(at),
            _ => {
                // Swap two delimiters, which keeps a document nearly well-formed.
                if let Some(found) = bytes[at..]
                    .iter()
                    .position(|&byte| matches!(byte, b'<' | b'>' | b'"' | b'&'))
                {
                    bytes[at + found] = b"<>\"&'/"[random.below(6)];
                }
            }
        }
    }
    bytes
}

fn collect(path: &Path, out: &mut Vec<std::path::PathBuf>) {
    if path.is_dir() {
        let mut entries: Vec<_> = std::fs::read_dir(path)
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()))
            .map(|entry| entry.unwrap_or_else(|error| panic!("{}: {error}", path.display())))
            // A link to a directory isn't followed, so that a link back up the tree can't make this go on forever.
            .filter(|entry| !entry.file_type().is_ok_and(|kind| kind.is_symlink()) || !entry.path().is_dir())
            .map(|entry| entry.path())
            .collect();
        entries.sort();
        for entry in entries {
            collect(&entry, out);
        }
    } else if std::fs::symlink_metadata(path)
        .unwrap_or_else(|error| panic!("{}: {error}", path.display()))
        .is_file()
    {
        out.push(path.to_path_buf());
    }
}

fn check(source: &[u8], options: Options, what: &dyn Fn() -> String) -> bool {
    let document = libxml_rust::parse(source, options);
    if let Err(problem) = verify(&document, source) {
        println!("FAILED: {problem}\n  in {}", what());
        return false;
    }
    true
}

fn main() {
    let mut mutations = 20;
    let mut max_size = 256 * 1024;
    let mut paths = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--mutations" => mutations = args.next().unwrap().parse().unwrap(),
            "--max-size" => max_size = args.next().unwrap().parse().unwrap(),
            _ => collect(Path::new(&arg), &mut paths),
        }
    }

    let resolver = |name: &[u8], out: &mut Vec<u8>| -> bool {
        if name == b"nbsp" {
            out.extend_from_slice("\u{a0}".as_bytes());
            return true;
        }
        false
    };
    let all_options = [
        Options {
            preserve_comments: true,
            preserve_cdata: true,
            resolve_html_entity: None,
        },
        Options {
            preserve_comments: false,
            preserve_cdata: false,
            resolve_html_entity: Some(&resolver),
        },
    ];

    let mut random = Random(0x9E37_79B9_7F4A_7C15);
    let (mut documents, mut failures) = (0u64, 0u64);
    for path in &paths {
        let source = std::fs::read(path).unwrap_or_else(|error| panic!("{}: {error}", path.display()));
        documents += 1;
        if !check(&source, all_options[0], &|| path.display().to_string()) {
            failures += 1;
        }
        if source.len() > max_size {
            continue;
        }
        for index in 0..mutations {
            let seed = random.0;
            let mutated = mutate(&mut random, &source);
            documents += 1;
            let options = all_options[index % 2];
            if !check(&mutated, options, &|| {
                format!("{} mutation {index} (state {seed:#x})", path.display())
            }) {
                failures += 1;
                let out = format!("xmlfuzz-failure-{failures}.xml");
                let _ = std::fs::write(&out, &mutated);
                println!("  written to {out}");
            }
        }
    }
    println!("{documents} documents, {failures} failures");
    // Nothing checked is no better than something failing: a mistyped path must not pass for a clean run.
    if failures != 0 || documents == 0 {
        std::process::exit(1);
    }
}
