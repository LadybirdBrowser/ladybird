/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What changed between two JSON Lines dumps of the display list of one document. Each dump builds the visual context
//! tree anew and names nodes and chunks by what the tape holds, so nodes are matched by their owner and role, records
//! by their owner, phase and command, and neither by an index.

use serde_json::{Map, Value};
use std::collections::{HashMap, HashSet};
use std::fmt::Write;

struct DumpedEntry {
    // The owner and role of a node, or the owner, phase and command of a record, with how many came before it.
    key: String,
    // What is compared, with the indices and the names that depend on them left out.
    content: String,
    // What the report prints of it.
    line: String,
}

#[derive(Default)]
struct ParsedDump {
    nodes: Vec<DumpedEntry>,
    records: Vec<DumpedEntry>,
    chunk_ids: Vec<String>,
}

// A node or resource name, such as s12, c3 or font2, as a placeholder, since names count up in print order.
fn without_numbered_names(text: &str) -> String {
    let mut result = String::with_capacity(text.len());
    let characters: Vec<char> = text.chars().collect();
    let mut index = 0;
    while index < characters.len() {
        let character = characters[index];
        let starts_word = index == 0 || !characters[index - 1].is_ascii_alphanumeric();
        if starts_word && character.is_ascii_alphabetic() {
            let word_end = (index..characters.len())
                .find(|&end| !characters[end].is_ascii_alphanumeric() && characters[end] != '_')
                .unwrap_or(characters.len());
            let word: String = characters[index..word_end].iter().collect();
            let letters_end = word.find(|c: char| c.is_ascii_digit()).unwrap_or(word.len());
            let (letters, digits) = word.split_at(letters_end);
            if !digits.is_empty() && digits.chars().all(|c| c.is_ascii_digit()) {
                result.push_str(letters);
                result.push('#');
            } else {
                result.push_str(&word);
            }
            index = word_end;
            continue;
        }
        result.push(character);
        index += 1;
    }
    result
}

// The raw indices and ids of the names in a value, which differ from one dump to the next.
fn without_raw_indices(value: &Value) -> Value {
    match value {
        Value::Object(object) => Value::Object(
            object
                .iter()
                .filter(|(key, _)| key.as_str() != "raw")
                .map(|(key, value)| (key.clone(), without_raw_indices(value)))
                .collect::<Map<_, _>>(),
        ),
        Value::Array(values) => Value::Array(values.iter().map(without_raw_indices).collect()),
        value => value.clone(),
    }
}

fn string_of(object: &Map<String, Value>, key: &str) -> String {
    object.get(key).and_then(Value::as_str).unwrap_or_default().to_string()
}

fn node_role(description: &str) -> &str {
    let end = description.find(['=', ' ', '(', '[']).unwrap_or(description.len());
    &description[..end]
}

fn parse_dump(jsonl: &str) -> ParsedDump {
    let mut dump = ParsedDump::default();
    let mut key_counts: HashMap<String, usize> = HashMap::new();
    let mut numbered_key = |key: String| {
        let count = key_counts.entry(key.clone()).or_default();
        *count += 1;
        format!("{key}|{count}")
    };
    for line in jsonl.lines() {
        let Ok(Value::Object(object)) = serde_json::from_str::<Value>(line) else {
            continue;
        };
        match object.get("type").and_then(Value::as_str) {
            Some("node") => {
                let (kind, owner, description) = (
                    string_of(&object, "kind"),
                    string_of(&object, "owner"),
                    string_of(&object, "description"),
                );
                let mut line = format!("[{}] {description}", string_of(&object, "name"));
                if !owner.is_empty() {
                    write!(line, " ({owner})").unwrap();
                }
                dump.nodes.push(DumpedEntry {
                    key: numbered_key(format!("node|{kind}|{owner}|{}", node_role(&description))),
                    content: without_numbered_names(&description),
                    line,
                });
            }
            Some("record") => {
                let (op, owner, phase) = (
                    string_of(&object, "op"),
                    string_of(&object, "owner"),
                    string_of(&object, "phase"),
                );
                let compared: Map<String, Value> = ["context", "fields", "inline_clips", "inline_transform", "text"]
                    .into_iter()
                    .filter_map(|key| Some((key.to_string(), without_raw_indices(object.get(key)?))))
                    .collect();
                let fields = object.get("fields").map(without_raw_indices).unwrap_or_default();
                let mut line = format!("{op} {fields}");
                if let Some(text) = object.get("text").and_then(Value::as_str) {
                    write!(line, " text={text:?}").unwrap();
                }
                write!(line, " owner={owner} phase={phase}").unwrap();
                dump.records.push(DumpedEntry {
                    key: numbered_key(format!("record|{op}|{owner}|{phase}")),
                    content: without_numbered_names(&Value::Object(compared).to_string()),
                    line,
                });
            }
            Some("chunk") => dump.chunk_ids.push(string_of(&object, "id")),
            _ => {}
        }
    }
    dump
}

fn report_entries(report: &mut String, label: &str, previous: &[DumpedEntry], current: &[DumpedEntry]) {
    let previous_by_key: HashMap<&str, &DumpedEntry> =
        previous.iter().map(|entry| (entry.key.as_str(), entry)).collect();
    let current_keys: HashSet<&str> = current.iter().map(|entry| entry.key.as_str()).collect();
    let mut lines = Vec::new();
    let (mut added, mut changed, mut unchanged) = (0, 0, 0);
    for entry in current {
        match previous_by_key.get(entry.key.as_str()) {
            None => {
                added += 1;
                lines.push(format!("  + {}", entry.line));
            }
            Some(previous_entry) if previous_entry.content != entry.content => {
                changed += 1;
                lines.push(format!("  ~ {}\n    was {}", entry.line, previous_entry.line));
            }
            Some(_) => unchanged += 1,
        }
    }
    let mut removed = 0;
    for entry in previous {
        if !current_keys.contains(entry.key.as_str()) {
            removed += 1;
            lines.push(format!("  - {}", entry.line));
        }
    }
    writeln!(
        report,
        "{label}: {added} added, {removed} removed, {changed} changed, {unchanged} unchanged"
    )
    .unwrap();
    for line in lines {
        writeln!(report, "{line}").unwrap();
    }
}

/// What changed from the dump `previous` to the dump `current`, both JSON Lines dumps of the display list of one
/// document, or a summary of `current` where there is no previous one.
pub(crate) fn display_list_dump_changes(previous: Option<&str>, current: &str) -> String {
    let current = parse_dump(current);
    let mut report = String::new();
    let Some(previous) = previous.map(parse_dump) else {
        writeln!(
            report,
            "No previous dump of this document. This one, of {} nodes, {} records and {} chunks, is what the next one is compared with.",
            current.nodes.len(),
            current.records.len(),
            current.chunk_ids.len()
        )
        .unwrap();
        return report;
    };
    report_entries(&mut report, "Nodes", &previous.nodes, &current.nodes);
    report_entries(&mut report, "Records", &previous.records, &current.records);
    let previous_chunk_ids: HashSet<&String> = previous.chunk_ids.iter().collect();
    let kept_chunk_count = current
        .chunk_ids
        .iter()
        .filter(|id| previous_chunk_ids.contains(id))
        .count();
    writeln!(
        report,
        "Chunks: {kept_chunk_count} of {} kept their id",
        current.chunk_ids.len()
    )
    .unwrap();
    report
}

#[cfg(test)]
mod tests {
    use super::*;

    const BEFORE: &str = concat!(
        r#"{"type":"node","kind":"spatial","name":"s0","raw":0,"depth":0,"description":"transform=[1,0,0,1,0,0] origin=(0,0)","owner":"Viewport<#document>"}"#,
        "\n",
        r#"{"type":"node","kind":"spatial","name":"s1","raw":4,"depth":1,"description":"transform=[1,0,0,1,5,0] origin=(0,0)","owner":"BlockContainer<DIV>#moved"}"#,
        "\n",
        r#"{"type":"chunk","name":"k1","id":"aa"}"#,
        "\n",
        r#"{"type":"record","id":1,"op":"FillRect","context":{"spatial":{"name":"s1","raw":4}},"fields":{"rect":[0,0,10,10]},"owner":"BlockContainer<DIV>#moved","phase":"background"}"#,
        "\n",
        r#"{"type":"record","id":2,"op":"FillRect","context":{"spatial":{"name":"s0","raw":0}},"fields":{"rect":[0,0,5,5]},"owner":"BlockContainer<DIV>#gone","phase":"background"}"#,
        "\n",
    );

    const AFTER: &str = concat!(
        r#"{"type":"node","kind":"spatial","name":"s0","raw":0,"depth":0,"description":"transform=[1,0,0,1,0,0] origin=(0,0)","owner":"Viewport<#document>"}"#,
        "\n",
        r#"{"type":"node","kind":"spatial","name":"s1","raw":2,"depth":1,"description":"transform=[1,0,0,1,9,0] origin=(0,0)","owner":"BlockContainer<DIV>#moved"}"#,
        "\n",
        r#"{"type":"chunk","name":"k1","id":"bb"}"#,
        "\n",
        r#"{"type":"record","id":1,"op":"FillRect","context":{"spatial":{"name":"s1","raw":2}},"fields":{"rect":[0,0,10,10]},"owner":"BlockContainer<DIV>#moved","phase":"background"}"#,
        "\n",
        r#"{"type":"record","id":2,"op":"DrawGlyphRun","context":{"spatial":{"name":"s0","raw":0}},"fields":{"glyphs":2},"text":"hi","owner":"BlockContainer<DIV>#new","phase":"foreground"}"#,
        "\n",
    );

    #[test]
    fn nodes_and_records_are_matched_by_owner_not_by_index() {
        assert_eq!(
            display_list_dump_changes(Some(BEFORE), AFTER),
            concat!(
                "Nodes: 0 added, 0 removed, 1 changed, 1 unchanged\n",
                "  ~ [s1] transform=[1,0,0,1,9,0] origin=(0,0) (BlockContainer<DIV>#moved)\n",
                "    was [s1] transform=[1,0,0,1,5,0] origin=(0,0) (BlockContainer<DIV>#moved)\n",
                "Records: 1 added, 1 removed, 0 changed, 1 unchanged\n",
                "  + DrawGlyphRun {\"glyphs\":2} text=\"hi\" owner=BlockContainer<DIV>#new phase=foreground\n",
                "  - FillRect {\"rect\":[0,0,5,5]} owner=BlockContainer<DIV>#gone phase=background\n",
                "Chunks: 0 of 1 kept their id\n",
            )
        );
    }

    #[test]
    fn a_first_dump_is_summarized() {
        assert_eq!(
            display_list_dump_changes(None, BEFORE),
            "No previous dump of this document. This one, of 2 nodes, 2 records and 1 chunks, is what the next one is compared with.\n"
        );
    }

    #[test]
    fn numbered_names_are_compared_without_their_numbers() {
        assert_eq!(
            without_numbered_names("sticky scroller=s12 parent_sticky=s3 in c40 font2 x1y"),
            "sticky scroller=s# parent_sticky=s# in c# font# x1y"
        );
    }
}
