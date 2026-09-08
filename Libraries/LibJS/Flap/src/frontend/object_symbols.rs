/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Extracts target C++ constant-expression values from symbols in a compiled probe object.

use std::path::Path;
use std::process::Command;

const SYMBOL_PREFIX: &str = "flap_layout_Q";

pub(crate) fn read_query_values(
    objdump_path: &Path,
    object_path: &Path,
    count: usize,
) -> Result<Vec<Option<u64>>, String> {
    let output = Command::new(objdump_path)
        .arg("-t")
        .arg(object_path)
        .output()
        .map_err(|error| format!("failed to run {}: {error}", objdump_path.display()))?;
    if !output.status.success() {
        return Err(format!(
            "{} could not read {}:\n{}{}",
            objdump_path.display(),
            object_path.display(),
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ));
    }

    let mut halves = vec![None; count];
    for line in String::from_utf8_lossy(&output.stdout).lines() {
        let tokens = line.split_ascii_whitespace().collect::<Vec<_>>();
        for (index, token) in tokens.iter().enumerate() {
            let Some((query, half)) = parse_symbol_token(token.trim_start_matches('_')) else {
                continue;
            };
            if let Some(slot) = halves.get_mut(query) {
                let value = symbol_value(&tokens, index)
                    .ok_or_else(|| format!("could not find symbol value in objdump line: {line}"))?;
                let value = value & 0xffff_ffff;
                let entry = slot.get_or_insert((None, None));
                match half {
                    Half::Low => entry.0 = Some(value),
                    Half::High => entry.1 = Some(value),
                }
            }
        }
    }

    Ok(halves
        .into_iter()
        .map(|entry| {
            let (low, high) = entry?;
            Some(low? | (high? << 32))
        })
        .collect())
}

#[derive(Clone, Copy)]
enum Half {
    Low,
    High,
}

fn parse_symbol_token(token: &str) -> Option<(usize, Half)> {
    let token = token.strip_prefix(SYMBOL_PREFIX)?;
    let (query, rest) = token.split_once("__")?;
    let (_, half) = rest.rsplit_once("__")?;
    let half = match half {
        "lo" => Half::Low,
        "hi" => Half::High,
        _ => return None,
    };
    Some((query.parse().ok()?, half))
}

fn parse_hex(token: &str) -> Option<u64> {
    let token = token.strip_prefix("0x").unwrap_or(token);
    (!token.is_empty() && token.bytes().all(|byte| byte.is_ascii_hexdigit()))
        .then(|| u64::from_str_radix(token, 16).ok())
        .flatten()
}

fn symbol_value(tokens: &[&str], symbol_index: usize) -> Option<u64> {
    let preceding = &tokens[..symbol_index];
    if preceding.contains(&"*ABS*") {
        return preceding.iter().find_map(|token| parse_hex(token));
    }
    preceding.iter().rev().find_map(|token| parse_hex(token))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_split_symbol_values() {
        let output = "\
0000000000000004 g       *ABS*\t0000000000000000 flap_layout_Q0__FIELD__lo\n\
0000000000000000 g       *ABS*\t0000000000000000 flap_layout_Q0__FIELD__hi\n\
fffffffffffffff8 g       *ABS*\t0000000000000000 flap_layout_Q1__NEGATIVE__lo\n\
ffffffffffffffff g       *ABS*\t0000000000000000 flap_layout_Q1__NEGATIVE__hi\n\
0000000000000020 g       *ABS*\t0000000000000000 _flap_layout_Q2__MACHO__lo\n\
0000000000000000 g       *ABS*\t0000000000000000 _flap_layout_Q2__MACHO__hi\n\
";
        let mut halves = vec![None; 3];
        for line in output.lines() {
            let tokens = line.split_ascii_whitespace().collect::<Vec<_>>();
            for (index, token) in tokens.iter().enumerate() {
                if let Some((query, half)) = parse_symbol_token(token.trim_start_matches('_')) {
                    let value = symbol_value(&tokens, index).unwrap() & 0xffff_ffff;
                    let entry = halves[query].get_or_insert((None, None));
                    match half {
                        Half::Low => entry.0 = Some(value),
                        Half::High => entry.1 = Some(value),
                    }
                }
            }
        }
        let values = halves
            .into_iter()
            .map(|entry| {
                let (low, high) = entry?;
                Some(low? | (high? << 32))
            })
            .collect::<Vec<_>>();
        assert_eq!(values, vec![Some(4), Some(0xffff_ffff_ffff_fff8), Some(32)]);
    }
}
