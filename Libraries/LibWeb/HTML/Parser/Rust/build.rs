/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Build script that generates a DAFSA (Deterministic Acyclic Finite State Automaton)
//! for named character reference matching. This is a Rust port of the C++ generator at
//! Meta/Lagom/Tools/CodeGenerators/LibWeb/GenerateNamedCharacterReferences.cpp.

use std::cell::RefCell;
use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

const FFI_HEADER: &str = "HTMLTokenizerRustFFI.h";

fn main() {
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=src");
    println!("cargo:rerun-if-env-changed=FFI_OUTPUT_DIR");

    let ffi_out_dir = env::var("FFI_OUTPUT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| out_dir.clone());

    cbindgen::generate(&manifest_dir).map_or_else(
        |error| match error {
            cbindgen::Error::ParseSyntaxError { .. } => {}
            e => panic!("{e:?}"),
        },
        |bindings| {
            bindings.write_to_file(out_dir.join(FFI_HEADER));
            if ffi_out_dir != out_dir {
                bindings.write_to_file(ffi_out_dir.join(FFI_HEADER));
            }
        },
    );

    // Generate interned name tables from the existing C++ headers.
    let tag_names_header = Path::new(&manifest_dir).join("../../TagNames.h");
    let attr_names_header = Path::new(&manifest_dir).join("../../AttributeNames.h");
    println!("cargo:rerun-if-changed={}", tag_names_header.display());
    println!("cargo:rerun-if-changed={}", attr_names_header.display());
    let tag_names = parse_enumerate_macro(
        &fs::read_to_string(&tag_names_header).expect("Failed to read TagNames.h"),
        "__ENUMERATE_HTML_TAG",
    );
    let attr_names = parse_enumerate_macro(
        &fs::read_to_string(&attr_names_header).expect("Failed to read AttributeNames.h"),
        "__ENUMERATE_HTML_ATTRIBUTE",
    );
    emit_interned_names(&out_dir.join("interned_names_generated.rs"), &tag_names, &attr_names);

    let json_path = Path::new(&manifest_dir).join("../Entities.json");
    println!("cargo:rerun-if-changed={}", json_path.display());

    let json_str = fs::read_to_string(&json_path).expect("Failed to read Entities.json");
    let entities = parse_entities_json(&json_str);

    // Build DAFSA.
    let mut builder = DafsaBuilder::new();
    for (name, _, _) in &entities {
        builder.insert(name);
    }
    builder.minimize(0);
    builder.calc_numbers();

    // Verify minimal perfect hashing (no collisions).
    let mut seen: Vec<bool> = vec![false; entities.len() + 1];
    for (name, _, _) in &entities {
        let idx = builder.get_unique_index(name).unwrap();
        assert!(!seen[idx], "Hash collision at index {idx} for '{name}'");
        seen[idx] = true;
    }

    // Build codepoints lookup table indexed by unique_index.
    let mut index_to_codepoints = vec![(0u32, 0u32); entities.len()];
    for (name, first, second) in &entities {
        let idx = builder.get_unique_index(name).unwrap();
        index_to_codepoints[idx - 1] = (*first, *second);
    }

    // Extract DAFSA layers.
    let root = &builder.root;
    let root_ref = root.borrow();

    let mut first_layer: Vec<u16> = Vec::new();
    let mut first_to_second_layer: Vec<(u64, u16)> = Vec::new();

    let mut first_layer_tally: u16 = 0;
    let mut second_layer_offset: u16 = 0;

    for c in 0u8..128 {
        if root_ref.children[c as usize].is_none() {
            continue;
        }
        assert!(c.is_ascii_alphabetic());
        let child = root_ref.children[c as usize].as_ref().unwrap();
        let child_ref = child.borrow();

        first_layer.push(first_layer_tally);
        first_layer_tally += child_ref.number;

        let mask = child_ref.get_ascii_alphabetic_bit_mask();
        first_to_second_layer.push((mask, second_layer_offset));
        second_layer_offset += child_ref.num_direct_children() as u16;
    }
    assert_eq!(first_layer.len(), 52);

    // BFS to build DAFSA node array.
    // Following the C++ write_node_data three-phase approach:
    type NodePtr = Rc<RefCell<Node>>;
    let mut queue: Vec<NodePtr> = Vec::new();
    let mut child_indexes: HashMap<*const RefCell<Node>, u16> = HashMap::new();

    // Phase 1: Queue root's children (first-layer nodes = 52 A-Z/a-z).
    // This assigns temporary child_indexes for first-layer children.
    queue_children(root, &mut queue, &mut child_indexes, 1);

    // Phase 2: Clear indexes and re-process. For each first-layer child,
    // queue ITS children (second-layer nodes) and assign their child_indexes.
    child_indexes.clear();
    let mut first_available_index: u16 = 1; // 0 is reserved (dummy node)
    let first_layer_count = queue.len();
    for i in 0..first_layer_count {
        let node = Rc::clone(&queue[i]);
        first_available_index = queue_children(&node, &mut queue, &mut child_indexes, first_available_index);
    }
    // Remove first-layer nodes from queue, keep only second-layer+ nodes.
    let second_layer_nodes: Vec<NodePtr> = queue.drain(first_layer_count..).collect();
    queue.clear();
    queue.extend(second_layer_nodes);

    // Phase 3: BFS remaining nodes, writing node data.
    let mut node_data: Vec<NodeData> = Vec::new();
    let mut qi = 0;
    #[allow(unused_assignments)]
    while qi < queue.len() {
        let node = Rc::clone(&queue[qi]);
        qi += 1;
        first_available_index = write_children_data(
            &node,
            &mut node_data,
            &mut queue,
            &mut child_indexes,
            first_available_index,
        );
    }

    // Build second_layer entries with child_indexes from phase 2.
    let mut second_layer: Vec<SecondLayerEntry> = Vec::new();
    for c in 0u8..128 {
        if root_ref.children[c as usize].is_none() {
            continue;
        }
        let first_child = root_ref.children[c as usize].as_ref().unwrap();
        let first_child_ref = first_child.borrow();
        let mut tally: u8 = 0;
        for cc in 0u8..128 {
            if first_child_ref.children[cc as usize].is_none() {
                continue;
            }
            let second_child = first_child_ref.children[cc as usize].as_ref().unwrap();
            let second_child_ref = second_child.borrow();
            let key = Rc::as_ptr(second_child);
            let ci = child_indexes.get(&key).copied().unwrap_or(0);
            let children_len = second_child_ref.num_direct_children();
            second_layer.push(SecondLayerEntry {
                child_index: ci,
                number: tally,
                children_len,
                end_of_word: second_child_ref.is_terminal,
            });
            tally = tally.wrapping_add(second_child_ref.number as u8);
        }
    }
    drop(root_ref);

    // Generate output file.
    let out_dir = env::var("OUT_DIR").unwrap();
    let out_path = Path::new(&out_dir).join("named_character_references.rs");
    let mut out = String::new();

    out.push_str("// Auto-generated by build.rs -- do not edit!\n\n");

    // Second codepoint enum.
    out.push_str("#[derive(Clone, Copy, PartialEq, Eq)]\n");
    out.push_str("#[repr(u8)]\n");
    out.push_str("pub enum SecondCodepoint {\n");
    out.push_str("    None = 0,\n");
    out.push_str("    CombiningLongSolidusOverlay = 1,\n");
    out.push_str("    CombiningLongVerticalLineOverlay = 2,\n");
    out.push_str("    HairSpace = 3,\n");
    out.push_str("    CombiningDoubleLowLine = 4,\n");
    out.push_str("    CombiningReverseSolidusOverlay = 5,\n");
    out.push_str("    VariationSelector1 = 6,\n");
    out.push_str("    LatinSmallLetterJ = 7,\n");
    out.push_str("    CombiningMacronBelow = 8,\n");
    out.push_str("}\n\n");

    out.push_str("impl SecondCodepoint {\n");
    out.push_str("    pub fn value(self) -> u32 {\n");
    out.push_str("        match self {\n");
    out.push_str("            SecondCodepoint::None => 0,\n");
    out.push_str("            SecondCodepoint::CombiningLongSolidusOverlay => 0x0338,\n");
    out.push_str("            SecondCodepoint::CombiningLongVerticalLineOverlay => 0x20D2,\n");
    out.push_str("            SecondCodepoint::HairSpace => 0x200A,\n");
    out.push_str("            SecondCodepoint::CombiningDoubleLowLine => 0x0333,\n");
    out.push_str("            SecondCodepoint::CombiningReverseSolidusOverlay => 0x20E5,\n");
    out.push_str("            SecondCodepoint::VariationSelector1 => 0xFE00,\n");
    out.push_str("            SecondCodepoint::LatinSmallLetterJ => 0x006A,\n");
    out.push_str("            SecondCodepoint::CombiningMacronBelow => 0x0331,\n");
    out.push_str("        }\n");
    out.push_str("    }\n");
    out.push_str("}\n\n");

    // Struct definitions.
    out.push_str("#[derive(Clone, Copy)]\n");
    out.push_str("pub struct DafsaNode {\n");
    out.push_str("    pub character: u8,\n");
    out.push_str("    pub number: u8,\n");
    out.push_str("    pub end_of_word: bool,\n");
    out.push_str("    pub child_index: u16,\n");
    out.push_str("    pub children_len: u8,\n");
    out.push_str("}\n\n");

    out.push_str("#[derive(Clone, Copy)]\n");
    out.push_str("pub struct SecondLayerNode {\n");
    out.push_str("    pub child_index: u16,\n");
    out.push_str("    pub number: u8,\n");
    out.push_str("    pub children_len: u8,\n");
    out.push_str("    pub end_of_word: bool,\n");
    out.push_str("}\n\n");

    // Codepoints lookup table.
    out.push_str(&format!(
        "pub static CODEPOINTS_LOOKUP: [(u32, SecondCodepoint); {}] = [\n",
        index_to_codepoints.len()
    ));
    for (first, second) in &index_to_codepoints {
        let variant = second_codepoint_variant(*second);
        out.push_str(&format!("    ({first:#06X}, SecondCodepoint::{variant}),\n"));
    }
    out.push_str("];\n\n");

    // DAFSA nodes array (with dummy node at index 0).
    out.push_str(&format!(
        "pub static DAFSA_NODES: [DafsaNode; {}] = [\n",
        node_data.len() + 1
    ));
    out.push_str("    DafsaNode { character: 0, number: 0, end_of_word: false, child_index: 0, children_len: 0 },\n");
    for nd in &node_data {
        out.push_str(&format!(
            "    DafsaNode {{ character: b'{}', number: {}, end_of_word: {}, child_index: {}, children_len: {} }},\n",
            escape_byte(nd.character),
            nd.number,
            nd.end_of_word,
            nd.child_index,
            nd.children_len
        ));
    }
    out.push_str("];\n\n");

    // First layer.
    out.push_str(&format!("pub static FIRST_LAYER: [u16; {}] = [\n", first_layer.len()));
    for n in &first_layer {
        out.push_str(&format!("    {n},\n"));
    }
    out.push_str("];\n\n");

    // First-to-second layer links.
    out.push_str(&format!(
        "pub static FIRST_TO_SECOND_LAYER: [(u64, u16); {}] = [\n",
        first_to_second_layer.len()
    ));
    for (mask, offset) in &first_to_second_layer {
        out.push_str(&format!("    ({mask:#018X}, {offset}),\n"));
    }
    out.push_str("];\n\n");

    // Second layer nodes.
    out.push_str(&format!(
        "pub static SECOND_LAYER: [SecondLayerNode; {}] = [\n",
        second_layer.len()
    ));
    for sl in &second_layer {
        out.push_str(&format!(
            "    SecondLayerNode {{ child_index: {}, number: {}, children_len: {}, end_of_word: {} }},\n",
            sl.child_index, sl.number, sl.children_len, sl.end_of_word
        ));
    }
    out.push_str("];\n\n");

    // Total entity count.
    out.push_str(&format!("pub const ENTITY_COUNT: usize = {};\n", entities.len()));

    fs::write(&out_path, &out).expect("Failed to write generated file");
}

/// Extract the string literal from `__ENUMERATE_FOO(ident, "string")` macro
/// invocations in a C++ header.
fn parse_enumerate_macro(source: &str, macro_name: &str) -> Vec<String> {
    let needle = format!("{macro_name}(");
    let mut out = Vec::new();
    for line in source.lines() {
        let Some(idx) = line.find(&needle) else {
            continue;
        };
        let rest = &line[idx + needle.len()..];
        // Take the second argument, which is the quoted string literal.
        let Some(first_quote) = rest.find('"') else {
            continue;
        };
        let after = &rest[first_quote + 1..];
        let Some(end_quote) = after.find('"') else {
            continue;
        };
        out.push(after[..end_quote].to_string());
    }
    out
}

/// Emit a Rust source file with two const byte-slice arrays and two lookup
/// functions that dispatch on length and then on the exact bytes. rustc
/// compiles this pattern to a jump table + direct memcmp, which beats a
/// HashMap lookup with a cryptographic default hasher by a wide margin for
/// the small, fixed set of HTML names.
fn emit_interned_names(out_path: &Path, tag_names: &[String], attr_names: &[String]) {
    let mut out = String::new();
    out.push_str("// Auto-generated by build.rs from TagNames.h / AttributeNames.h.\n");
    out.push_str("// Do not edit by hand.\n\n");

    out.push_str("pub const INTERNED_TAG_NAMES: &[&[u8]] = &[\n");
    for name in tag_names {
        out.push_str(&format!("    b\"{}\",\n", name));
    }
    out.push_str("];\n\n");

    out.push_str("pub const INTERNED_ATTR_NAMES: &[&[u8]] = &[\n");
    for name in attr_names {
        out.push_str(&format!("    b\"{}\",\n", name));
    }
    out.push_str("];\n\n");

    emit_lookup_fn(&mut out, "lookup_tag_name_generated", tag_names);
    emit_lookup_fn(&mut out, "lookup_attr_name_generated", attr_names);

    fs::write(out_path, out).expect("Failed to write interned_names_generated.rs");
}

fn emit_lookup_fn(out: &mut String, fn_name: &str, names: &[String]) {
    // Group names by byte length so the outer dispatch can be a single match.
    let mut by_length: std::collections::BTreeMap<usize, Vec<(usize, &String)>> = std::collections::BTreeMap::new();
    for (i, name) in names.iter().enumerate() {
        by_length.entry(name.len()).or_default().push((i, name));
    }

    out.push_str(&format!("#[inline]\npub fn {fn_name}(bytes: &[u8]) -> u16 {{\n"));
    out.push_str("    match bytes.len() {\n");
    for (length, entries) in &by_length {
        out.push_str(&format!("        {length} => match bytes {{\n"));
        for (index, name) in entries {
            // id is 1-based.
            let id = index + 1;
            out.push_str(&format!("            b\"{name}\" => {id},\n"));
        }
        out.push_str("            _ => 0,\n");
        out.push_str("        },\n");
    }
    out.push_str("        _ => 0,\n");
    out.push_str("    }\n");
    out.push_str("}\n\n");
}

fn escape_byte(b: u8) -> String {
    if b == b'\'' {
        "\\'".to_string()
    } else if b == b'\\' {
        "\\\\".to_string()
    } else if b.is_ascii_graphic() || b == b' ' {
        String::from(b as char)
    } else {
        format!("\\x{b:02X}")
    }
}

fn second_codepoint_variant(cp: u32) -> &'static str {
    match cp {
        0 => "None",
        0x0338 => "CombiningLongSolidusOverlay",
        0x20D2 => "CombiningLongVerticalLineOverlay",
        0x200A => "HairSpace",
        0x0333 => "CombiningDoubleLowLine",
        0x20E5 => "CombiningReverseSolidusOverlay",
        0xFE00 => "VariationSelector1",
        0x006A => "LatinSmallLetterJ",
        0x0331 => "CombiningMacronBelow",
        _ => panic!("Unknown second codepoint: {cp:#X}"),
    }
}

// Minimal JSON parser for Entities.json.
fn parse_entities_json(json: &str) -> Vec<(String, u32, u32)> {
    let mut entities = Vec::new();
    let bytes = json.as_bytes();
    let len = bytes.len();
    let mut i = 0;

    while i < len && bytes[i] != b'{' {
        i += 1;
    }
    i += 1;

    loop {
        while i < len && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= len || bytes[i] == b'}' {
            break;
        }
        if bytes[i] == b',' {
            i += 1;
            continue;
        }

        // Parse key.
        assert_eq!(bytes[i], b'"');
        i += 1;
        let key_start = i;
        while i < len && bytes[i] != b'"' {
            if bytes[i] == b'\\' {
                i += 1;
            }
            i += 1;
        }
        let key = std::str::from_utf8(&bytes[key_start..i]).unwrap().to_string();
        i += 1;

        // Skip ':'.
        while i < len && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        assert_eq!(bytes[i], b':');
        i += 1;

        // Skip to inner '{'.
        while i < len && bytes[i] != b'{' {
            i += 1;
        }
        i += 1;

        // Parse inner object for "codepoints".
        let mut codepoints: Vec<u32> = Vec::new();
        while i < len && bytes[i] != b'}' {
            if bytes[i] == b'"' {
                i += 1;
                let field_start = i;
                while i < len && bytes[i] != b'"' {
                    i += 1;
                }
                let field_name = std::str::from_utf8(&bytes[field_start..i]).unwrap();
                i += 1;

                while i < len && bytes[i].is_ascii_whitespace() {
                    i += 1;
                }
                assert_eq!(bytes[i], b':');
                i += 1;
                while i < len && bytes[i].is_ascii_whitespace() {
                    i += 1;
                }

                if field_name == "codepoints" {
                    assert_eq!(bytes[i], b'[');
                    i += 1;
                    loop {
                        while i < len && (bytes[i].is_ascii_whitespace() || bytes[i] == b',') {
                            i += 1;
                        }
                        if i >= len || bytes[i] == b']' {
                            i += 1;
                            break;
                        }
                        let num_start = i;
                        while i < len && bytes[i].is_ascii_digit() {
                            i += 1;
                        }
                        let num_str = std::str::from_utf8(&bytes[num_start..i]).unwrap();
                        codepoints.push(num_str.parse().unwrap());
                    }
                } else {
                    // Skip value.
                    if bytes[i] == b'"' {
                        i += 1;
                        while i < len && bytes[i] != b'"' {
                            if bytes[i] == b'\\' {
                                i += 1;
                            }
                            i += 1;
                        }
                        i += 1;
                    } else if bytes[i] == b'[' {
                        let mut depth = 1;
                        i += 1;
                        while i < len && depth > 0 {
                            if bytes[i] == b'[' {
                                depth += 1;
                            } else if bytes[i] == b']' {
                                depth -= 1;
                            }
                            i += 1;
                        }
                    }
                }
            } else {
                i += 1;
            }
        }
        i += 1;

        let name = key.strip_prefix('&').unwrap_or(&key).to_string();
        let first = codepoints.first().copied().unwrap_or(0);
        let second = if codepoints.len() > 1 { codepoints[1] } else { 0 };
        entities.push((name, first, second));
    }

    entities.sort_by(|a, b| a.0.cmp(&b.0));
    entities
}

// DAFSA builder using Rc<RefCell<Node>> for shared ownership.

type NodeRc = Rc<RefCell<Node>>;

struct Node {
    children: Vec<Option<NodeRc>>, // 128 slots
    is_terminal: bool,
    number: u16,
}

struct SecondLayerEntry {
    child_index: u16,
    number: u8,
    children_len: u8,
    end_of_word: bool,
}

struct NodeData {
    character: u8,
    number: u8,
    end_of_word: bool,
    child_index: u16,
    children_len: u8,
}

impl Node {
    fn new_rc() -> NodeRc {
        Rc::new(RefCell::new(Node {
            children: (0..128).map(|_| Option::None).collect(),
            is_terminal: false,
            number: 0,
        }))
    }

    fn calc_numbers(&mut self) {
        self.number = if self.is_terminal { 1 } else { 0 };
        for child in self.children.iter().flatten() {
            child.borrow_mut().calc_numbers();
            self.number += child.borrow().number;
        }
    }

    fn num_direct_children(&self) -> u8 {
        let mut n = 0u8;
        for c in &self.children {
            if c.is_some() {
                n += 1;
            }
        }
        n
    }

    fn get_ascii_alphabetic_bit_mask(&self) -> u64 {
        let mut mask = 0u64;
        for i in 0..128u8 {
            if self.children[i as usize].is_some() {
                mask |= 1u64 << ascii_alphabetic_to_index(i);
            }
        }
        mask
    }

    /// Hash based on child identities (Rc pointer) and terminal status.
    fn structure_hash(&self) -> u64 {
        let mut h: u64 = if self.is_terminal { 1 } else { 0 };
        for (i, child) in self.children.iter().enumerate() {
            if let Some(c) = child {
                h = h.wrapping_mul(31).wrapping_add(i as u64);
                h = h.wrapping_mul(31).wrapping_add(Rc::as_ptr(c) as u64);
            }
        }
        h
    }

    /// Check structural equality via Rc pointer identity.
    fn structure_eq(&self, other: &Node) -> bool {
        if self.is_terminal != other.is_terminal {
            return false;
        }
        for i in 0..128 {
            match (&self.children[i], &other.children[i]) {
                (None, None) => {}
                (Some(a), Some(b)) => {
                    if !Rc::ptr_eq(a, b) {
                        return false;
                    }
                }
                _ => return false,
            }
        }
        true
    }
}

fn ascii_alphabetic_to_index(c: u8) -> u8 {
    if c <= b'Z' { c - b'A' } else { c - b'a' + 26 }
}

struct UncheckedNode {
    parent: NodeRc,
    character: u8,
}

struct DafsaBuilder {
    root: NodeRc,
    minimized_nodes: HashMap<u64, Vec<NodeRc>>,
    unchecked_nodes: Vec<UncheckedNode>,
    previous_word: String,
}

impl DafsaBuilder {
    fn new() -> Self {
        DafsaBuilder {
            root: Node::new_rc(),
            minimized_nodes: HashMap::new(),
            unchecked_nodes: Vec::new(),
            previous_word: String::new(),
        }
    }

    fn insert(&mut self, word: &str) {
        assert!(
            word > self.previous_word.as_str(),
            "Words must be inserted in sorted order: '{word}' <= '{}'",
            self.previous_word
        );

        let common_prefix_len = word
            .bytes()
            .zip(self.previous_word.bytes())
            .take_while(|(a, b)| a == b)
            .count();

        self.minimize(common_prefix_len);

        let node: NodeRc = if self.unchecked_nodes.is_empty() {
            Rc::clone(&self.root)
        } else {
            let last = &self.unchecked_nodes[self.unchecked_nodes.len() - 1];
            let parent = last.parent.borrow();
            Rc::clone(parent.children[last.character as usize].as_ref().unwrap())
        };

        let remaining = &word[common_prefix_len..];
        let mut current = node;
        for c in remaining.bytes() {
            let new_child = Node::new_rc();
            {
                let mut current_ref = current.borrow_mut();
                assert!(current_ref.children[c as usize].is_none());
                current_ref.children[c as usize] = Some(Rc::clone(&new_child));
            }
            self.unchecked_nodes.push(UncheckedNode {
                parent: Rc::clone(&current),
                character: c,
            });
            current = new_child;
        }
        current.borrow_mut().is_terminal = true;

        self.previous_word = word.to_string();
    }

    fn minimize(&mut self, down_to: usize) {
        while self.unchecked_nodes.len() > down_to {
            let unchecked = self.unchecked_nodes.pop().unwrap();
            let parent = &unchecked.parent;
            let child = {
                let parent_ref = parent.borrow();
                Rc::clone(parent_ref.children[unchecked.character as usize].as_ref().unwrap())
            };

            let hash = child.borrow().structure_hash();
            let mut found_replacement: Option<NodeRc> = Option::None;

            if let Some(bucket) = self.minimized_nodes.get(&hash) {
                for existing in bucket {
                    if child.borrow().structure_eq(&existing.borrow()) {
                        found_replacement = Some(Rc::clone(existing));
                        break;
                    }
                }
            }

            if let Some(replacement) = found_replacement {
                parent.borrow_mut().children[unchecked.character as usize] = Some(replacement);
            } else {
                self.minimized_nodes.entry(hash).or_default().push(Rc::clone(&child));
            }
        }
    }

    fn calc_numbers(&mut self) {
        self.root.borrow_mut().calc_numbers();
    }

    fn get_unique_index(&self, word: &str) -> Option<usize> {
        let mut index: usize = 0;
        let mut current = Rc::clone(&self.root);

        for c in word.bytes() {
            let next = {
                let node = current.borrow();
                let child = node.children[c as usize].as_ref()?;
                for sibling_c in 0u8..128 {
                    if let Some(sibling) = &node.children[sibling_c as usize]
                        && sibling_c < c
                    {
                        index += sibling.borrow().number as usize;
                    }
                }
                Rc::clone(child)
            };
            if next.borrow().is_terminal {
                index += 1;
            }
            current = next;
        }

        Some(index)
    }
}

fn queue_children(
    node: &NodeRc,
    queue: &mut Vec<NodeRc>,
    child_indexes: &mut HashMap<*const RefCell<Node>, u16>,
    first_available_index: u16,
) -> u16 {
    let mut current = first_available_index;
    let node_ref = node.borrow();
    for c in 0..128u8 {
        if let Some(child) = &node_ref.children[c as usize] {
            let key = Rc::as_ptr(child);
            if let std::collections::hash_map::Entry::Vacant(entry) = child_indexes.entry(key) {
                let num_children = child.borrow().num_direct_children();
                if num_children > 0 {
                    entry.insert(current);
                    current += num_children as u16;
                }
                queue.push(Rc::clone(child));
            }
        }
    }
    current
}

fn write_children_data(
    node: &NodeRc,
    node_data: &mut Vec<NodeData>,
    queue: &mut Vec<NodeRc>,
    child_indexes: &mut HashMap<*const RefCell<Node>, u16>,
    first_available_index: u16,
) -> u16 {
    let mut current = first_available_index;
    let mut unique_index_tally: u8 = 0;
    let node_ref = node.borrow();
    for c in 0..128u8 {
        if let Some(child) = &node_ref.children[c as usize] {
            let key = Rc::as_ptr(child);
            let child_ref = child.borrow();
            let num_children = child_ref.num_direct_children();

            if let std::collections::hash_map::Entry::Vacant(entry) = child_indexes.entry(key) {
                if num_children > 0 {
                    entry.insert(current);
                    current += num_children as u16;
                }
                queue.push(Rc::clone(child));
            }

            node_data.push(NodeData {
                character: c,
                number: unique_index_tally,
                end_of_word: child_ref.is_terminal,
                child_index: child_indexes.get(&key).copied().unwrap_or(0),
                children_len: num_children,
            });

            unique_index_tally = unique_index_tally.wrapping_add(child_ref.number as u8);
        }
    }
    current
}
