/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Print whether each module named on stdin decodes and validates, one `path<TAB>verdict` line per module, for
//! comparing verdicts with other implementations. With `--stats`, valid modules also report the size of their code
//! and of what validation keeps for running it.

use std::io::BufRead;

fn main() {
    let stats = std::env::args().any(|argument| argument == "--stats");
    for line in std::io::stdin().lock().lines() {
        let path = line.expect("read a path");
        let verdict = match std::fs::read(&path) {
            Err(error) => format!("unreadable: {error}"),
            Ok(bytes) => match libwasm_engine::compile(&bytes) {
                Ok(module) if stats => {
                    let code: u32 = module.code.iter().map(|body| body.code.end - body.code.start).sum();
                    let entries: usize = module.function_infos.iter().map(|info| info.side_table.len()).sum();
                    let metadata: usize = module.function_infos.iter().map(|info| info.metadata_size()).sum();
                    format!(
                        "valid functions={} code_bytes={code} side_table_entries={entries} metadata_bytes={metadata}",
                        module.code.len()
                    )
                }
                Ok(_) => "valid".to_string(),
                Err(error) => format!("error:{error}"),
            },
        };
        println!("{path}\t{verdict}");
    }
}
