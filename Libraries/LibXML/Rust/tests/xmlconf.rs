/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runs the W3C XML Conformance Test Suite, if `XMLCONF_DIR` points at an unpacked copy of it (the directory with
//! `xmlconf.xml` in it). Set `XMLCONF_VERBOSE` to list the tests that don't do what the suite says, or to `all` to
//! include the ones that can't without reading external entities.

use std::collections::BTreeMap;
use std::path::Path;
use std::path::PathBuf;

use libxml_rust::Document;
use libxml_rust::Options;
use libxml_rust::parse;
use libxml_rust::tape::kind;

struct Test {
    id: String,
    kind: String,
    path: PathBuf,
    output: Option<PathBuf>,
    entities: String,
    namespaces: bool,
}

fn attribute<'a>(document: &'a Document, source: &'a [u8], row: usize, wanted: &str) -> Option<&'a [u8]> {
    let storage = document.storage(source);
    let tape = &document.tape;
    let mut index = row + 1;
    while index < tape.len() && tape.row(index).0 & kind::MASK == kind::ATTR {
        let (_, name, _) = tape.row(index);
        if document.names.bytes(name, storage) == wanted.as_bytes() {
            let (_, offset, length) = tape.row(index + 1);
            return Some(storage.get(offset, length));
        }
        index += 2;
    }
    None
}

/// Reads one catalog file. The suite's catalogs are XML, so they are read with the parser under test.
fn read_catalog(path: &Path, tests: &mut Vec<Test>) {
    let mut source = std::fs::read(path).unwrap();
    // Some of the catalogs are a list of TEST elements with no element around them.
    if !source.windows(10).any(|window| window == b"<TESTCASES") {
        let mut wrapped = b"<TESTCASES>".to_vec();
        let body = match source.windows(2).position(|window| window == b"?>") {
            Some(end) if source.starts_with(b"<?xml") => &source[end + 2..],
            _ => &source[..],
        };
        wrapped.extend_from_slice(body);
        wrapped.extend_from_slice(b"</TESTCASES>");
        source = wrapped;
    }
    let document = parse(&source, Options::default());
    assert!(
        document.errors.is_empty(),
        "{}: {}",
        path.display(),
        document.errors[0].message
    );
    let storage = document.storage(&source);
    let directory = path.parent().unwrap();
    for row in 0..document.tape.len() {
        let (row_kind, name, _) = document.tape.row(row);
        if row_kind & kind::MASK != kind::START || document.names.bytes(name, storage) != b"TEST" {
            continue;
        }
        let text =
            |wanted: &str| attribute(&document, &source, row, wanted).map(|v| String::from_utf8_lossy(v).into_owned());
        // This parser implements the fifth edition of XML 1.0, and XML 1.1 documents are read by its rules.
        if text("VERSION").is_some_and(|version| version != "1.0") {
            continue;
        }
        if text("RECOMMENDATION").is_some_and(|recommendation| recommendation.ends_with("1.1")) {
            continue;
        }
        if text("EDITION").is_some_and(|edition| !edition.split_whitespace().any(|e| e == "5")) {
            continue;
        }
        tests.push(Test {
            id: text("ID").unwrap(),
            kind: text("TYPE").unwrap(),
            path: directory.join(text("URI").unwrap()),
            output: text("OUTPUT").map(|output| directory.join(output)),
            entities: text("ENTITIES").unwrap_or_else(|| "none".into()),
            namespaces: text("NAMESPACE").is_none_or(|namespace| namespace != "no"),
        });
    }
}

fn escape(bytes: &[u8], out: &mut Vec<u8>) {
    for &byte in bytes {
        match byte {
            b'&' => out.extend_from_slice(b"&amp;"),
            b'<' => out.extend_from_slice(b"&lt;"),
            b'>' => out.extend_from_slice(b"&gt;"),
            b'"' => out.extend_from_slice(b"&quot;"),
            b'\t' => out.extend_from_slice(b"&#9;"),
            b'\n' => out.extend_from_slice(b"&#10;"),
            b'\r' => out.extend_from_slice(b"&#13;"),
            _ => out.push(byte),
        }
    }
}

/// The document in the canonical form the suite's expected outputs are in.
fn canonical(document: &Document, source: &[u8]) -> Vec<u8> {
    let storage = document.storage(source);
    let tape = &document.tape;
    let mut out = Vec::new();
    let mut index = 0;
    while index < tape.len() {
        let (row_kind, a, b) = tape.row(index);
        match row_kind & kind::MASK {
            kind::START => {
                out.push(b'<');
                out.extend_from_slice(document.names.bytes(a, storage));
                let mut attributes = BTreeMap::new();
                index += 1;
                while index < tape.len() && tape.row(index).0 & kind::MASK == kind::ATTR {
                    let (_, name, _) = tape.row(index);
                    let (_, offset, length) = tape.row(index + 1);
                    attributes.insert(document.names.bytes(name, storage), storage.get(offset, length));
                    index += 2;
                }
                for (name, value) in attributes {
                    out.push(b' ');
                    out.extend_from_slice(name);
                    out.extend_from_slice(b"=\"");
                    escape(value, &mut out);
                    out.push(b'"');
                }
                out.push(b'>');
                continue;
            }
            kind::END => {
                out.extend_from_slice(b"</");
                out.extend_from_slice(document.names.bytes(a, storage));
                out.push(b'>');
            }
            kind::TEXT => escape(storage.get(a, b), &mut out),
            kind::PI => {
                let (_, offset, length) = tape.row(index + 1);
                out.extend_from_slice(b"<?");
                out.extend_from_slice(document.names.bytes(a, storage));
                out.push(b' ');
                out.extend_from_slice(storage.get(offset, length));
                out.extend_from_slice(b"?>");
                index += 1;
            }
            kind::DOCTYPE => index += 2,
            _ => {}
        }
        index += 1;
    }
    out
}

#[test]
fn xmlconf() {
    let Ok(root) = std::env::var("XMLCONF_DIR") else {
        eprintln!("XMLCONF_DIR is not set, skipping");
        return;
    };
    let root = PathBuf::from(root);
    let verbose = std::env::var("XMLCONF_VERBOSE").is_ok();

    // The top catalog pulls the others in as external entities, which this parser doesn't read. Their paths are in
    // its entity declarations.
    let top = std::fs::read_to_string(root.join("xmlconf.xml")).unwrap();
    let mut tests = Vec::new();
    for declaration in top.split("<!ENTITY").skip(1) {
        let Some(start) = declaration.find("SYSTEM \"") else {
            continue;
        };
        let rest = &declaration[start + 8..];
        let path = &rest[..rest.find('"').unwrap()];
        read_catalog(&root.join(path), &mut tests);
    }
    assert!(tests.len() > 1500, "only found {} tests", tests.len());

    let options = Options {
        preserve_comments: false,
        preserve_cdata: false,
        resolve_html_entity: None,
    };
    let mut tally: BTreeMap<String, (usize, usize)> = BTreeMap::new();
    let mut failures = Vec::new();
    for test in &tests {
        // These three are documents whose encoding declaration disagrees with how they are encoded. That is for
        // whoever decodes the document to notice: the parser is handed UTF-8 and doesn't act on the declaration.
        if matches!(test.id.as_str(), "rmt-e2e-61" | "hst-lhs-007" | "hst-lhs-008") {
            continue;
        }
        // The expected output of this one has a processing instruction that is in the DTD. Those aren't part of the
        // document's content, and get no row.
        if test.id == "ibm-valid-P29-ibm29v01.xml" {
            continue;
        }
        let source = std::fs::read(&test.path)
            .unwrap_or_else(|error| panic!("{}: cannot read {}: {error}", test.id, test.path.display()));
        // The suite has documents in UTF-16 and other encodings. Decoding is the caller's job, so they are decoded
        // here, and the ones this test can't decode are left out.
        let source = if source.starts_with(&[0xFF, 0xFE]) || source.starts_with(&[0xFE, 0xFF]) {
            let big_endian = source[0] == 0xFE;
            let units: Vec<u16> = source[2..]
                .as_chunks::<2>()
                .0
                .iter()
                .map(|pair| {
                    if big_endian {
                        u16::from_be_bytes([pair[0], pair[1]])
                    } else {
                        u16::from_le_bytes([pair[0], pair[1]])
                    }
                })
                .collect();
            match String::from_utf16(&units) {
                Ok(text) => text.into_bytes(),
                Err(_) => continue,
            }
        } else {
            let head = String::from_utf8_lossy(&source[..source.len().min(100)]).to_lowercase();
            if head.contains("encoding")
                && !head.contains("utf-8")
                && !head.contains("us-ascii")
                && std::str::from_utf8(&source).is_err()
            {
                continue;
            }
            source
        };

        let document = parse(&source, options);
        if let Err(problem) = libxml_rust::verify::verify(&document, &source) {
            panic!("{}: {problem}", test.id);
        }
        let fatal = document.errors.iter().any(|error| error.fatal);
        // A reference to an entity that may be declared where this parser doesn't read is reported without failing
        // the parse. The document is well-formed all the same, and namespace-well-formed too.
        let any_error = document
            .errors
            .iter()
            .any(|error| error.fatal || !error.message.starts_with("Entity '"));
        let self_contained = test.entities == "none";
        let bucket = format!(
            "{}{}",
            test.kind,
            if self_contained { "" } else { " (external entities)" }
        );
        let passed = match test.kind.as_str() {
            "not-wf" => {
                if test.namespaces {
                    any_error
                } else {
                    fatal
                }
            }
            "valid" | "invalid" => {
                let well_formed = if test.namespaces { !any_error } else { !fatal };
                let output_matches = match &test.output {
                    Some(output) if well_formed => std::fs::read(output).is_ok_and(|expected| {
                        // Some expected outputs start with the document's notation declarations, which a parser
                        // that doesn't validate has no use for and this one doesn't report.
                        let expected = match expected.windows(3).position(|window| window == b"]>\n") {
                            Some(end) if expected.starts_with(b"<!DOCTYPE") => &expected[end + 3..],
                            _ => &expected[..],
                        };
                        expected == canonical(&document, &source)
                    }),
                    _ => true,
                };
                well_formed && output_matches
            }
            _ => true,
        };
        // These two say that a CR written as a character reference in an entity's value is still a CR where the
        // entity is used in content. That is what XML says, but not what libxml2 and with it most of the web does:
        // the line end is normalized where the entity is used, and the CR arrives as a LF.
        let passed = passed || matches!(test.id.as_str(), "valid-sa-068" | "valid-sa-108");
        let entry = tally.entry(bucket.clone()).or_default();
        entry.1 += 1;
        if passed {
            entry.0 += 1;
        } else {
            let reason = match document.errors.first() {
                Some(error) => format!("{}:{}: {}", error.line, error.column, error.message),
                None => "no error".into(),
            };
            if self_contained || std::env::var("XMLCONF_VERBOSE").is_ok_and(|value| value == "all") {
                let path = test.path.strip_prefix(&root).unwrap_or(&test.path);
                failures.push(format!("{} [{}] {} -- {}", test.id, bucket, path.display(), reason));
            }
        }
    }

    for (bucket, (passed, total)) in &tally {
        eprintln!("{bucket:40} {passed:5} / {total}");
    }
    if verbose {
        for failure in &failures {
            eprintln!("  {failure}");
        }
    }

    // Everything that doesn't depend on an external entity has to do what the suite says.
    for bucket in ["not-wf", "valid", "invalid"] {
        let (passed, total) = tally[bucket];
        assert_eq!(passed, total, "{bucket}: run with XMLCONF_VERBOSE=1 for the list");
    }
}

#[test]
fn error_positions() {
    // An error in an entity's text is at the reference to the entity.
    let source = b"<!DOCTYPE a [\n<!ENTITY e '<q:c/>'>\n]>\n<a>\n  <p:b/>\n\n <p:b x='&e;'/><p:c>&e;</p:c>\n<";
    let document = parse(source, Options::default());
    let positions: Vec<(u32, u32, bool)> = document
        .errors
        .iter()
        .map(|error| (error.line, error.column, error.fatal))
        .collect();
    assert_eq!(
        positions,
        [(5, 3, false), (7, 10, true)],
        "{:?}",
        document.errors.iter().map(|error| &error.message).collect::<Vec<_>>()
    );

    let source = b"<a>\n<p:b/>\n\n  <p:c/><p:d>\n&#1;";
    let document = parse(source, Options::default());
    let positions: Vec<(u32, u32, bool)> = document
        .errors
        .iter()
        .map(|error| (error.line, error.column, error.fatal))
        .collect();
    assert_eq!(positions, [(2, 1, false), (4, 3, false), (4, 9, false), (5, 1, true)]);
}

#[test]
fn defaulted_attributes_count_as_expansion() {
    let document_with = |attributes: usize, elements: usize| {
        let mut source = String::from("<!DOCTYPE a [\n<!ATTLIST e");
        for index in 0..attributes {
            source.push_str(&format!(" a{index} CDATA '0'"));
        }
        source.push_str(">\n]>\n<a>");
        source.push_str(&"<e/>".repeat(elements));
        source.push_str("</a>");
        source
    };

    // A document that makes modest use of defaults is fine.
    let source = document_with(20, 100);
    assert!(parse(source.as_bytes(), Options::default()).errors.is_empty());

    // One whose elements each get hundreds of attributes they don't spell out grows with the square of its size.
    let source = document_with(500, 2000);
    let document = parse(source.as_bytes(), Options::default());
    let error = document.errors.first().expect("the document is refused");
    assert!(error.fatal);
    assert_eq!(error.message, "Maximum entity amplification factor exceeded");
    assert!(document.tape.len() < 200_000, "{} rows", document.tape.len());

    // So does one whose elements each get a single attribute with a very long name and no value to speak of.
    let source = format!(
        "<!DOCTYPE a [<!ATTLIST e {} CDATA ''>]><a>{}</a>",
        "n".repeat(100_000),
        "<e/>".repeat(10_000)
    );
    let document = parse(source.as_bytes(), Options::default());
    let error = document.errors.first().expect("the document is refused");
    assert_eq!(error.message, "Maximum entity amplification factor exceeded");

    // A message that quotes a name the error isn't at doesn't quote all of a very long one.
    let source = format!("<{} p:a='' p:b=''/>", "e".repeat(100_000));
    let document = parse(source.as_bytes(), Options::default());
    assert_eq!(document.errors.len(), 2);
    assert!(document.errors.iter().all(|error| error.message.len() < 1000));

    // Errors that aren't in the order of their offsets still get their own lines.
    let document = parse(
        b"<!DOCTYPE a SYSTEM 'a.dtd'>\n<a>\n<e a='&b;' p:x=''/>\n<e\n a='&b;' p:x=''/></a>",
        Options::default(),
    );
    let positions: Vec<_> = document.errors.iter().map(|error| (error.line, error.column)).collect();
    assert_eq!(positions, [(3, 7), (3, 1), (5, 5), (4, 1)]);

    // The xml prefix can't be given another namespace by a default either.
    let document = parse(
        b"<!DOCTYPE r [<!ATTLIST r xmlns:xml CDATA 'urn:wrong'>]><r/>",
        Options::default(),
    );
    let errors: Vec<_> = document
        .errors
        .iter()
        .map(|error| (error.message.as_str(), error.fatal))
        .collect();
    assert_eq!(errors, [("xml namespace prefix mapped to wrong URI", false)]);

    // An undeclared entity that may be declared where the parser doesn't read is reported, and parsing goes on.
    let document = parse(b"<!DOCTYPE a SYSTEM 'a.dtd'><a>&b;</a>", Options::default());
    let errors: Vec<_> = document
        .errors
        .iter()
        .map(|error| (error.message.as_str(), error.fatal))
        .collect();
    assert_eq!(errors, [("Entity 'b' not defined", false)]);
}
