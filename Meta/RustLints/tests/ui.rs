/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiles each tests/ui/*.rs with the driver against stand-ins for libjs_rust and smallvec, and checks the diagnostics against
//! the file's annotations: `//~ ERROR text` for a diagnostic on the same line, `//~^ ERROR text` for the line above.

use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const DRIVER: &str = env!("CARGO_BIN_EXE_ladybird-rust-lints");

fn run_driver(args: &[&str]) -> String {
    let output = Command::new(DRIVER).args(args).output().expect("run the driver");
    String::from_utf8_lossy(&output.stderr).into_owned()
}

fn expected_diagnostics(source: &str) -> Vec<(usize, String, String)> {
    let mut expected = Vec::new();
    for (index, line) in source.lines().enumerate() {
        for annotation in line.split("//~").skip(1) {
            let carets = annotation.chars().take_while(|&c| c == '^').count();
            let mut words = annotation[carets..].trim_start().splitn(2, ' ');
            let level = match words.next().unwrap_or_default() {
                "WARN" => "warning".to_owned(),
                level => level.to_lowercase(),
            };
            let text = words.next().unwrap_or_default().trim().to_owned();
            expected.push((index + 1 - carets, level, text));
        }
    }
    expected
}

// With --error-format=short, each diagnostic is one line: "path:line:column: level: message".
fn actual_diagnostics(stderr: &str, file: &str) -> Vec<(usize, String, String)> {
    let mut actual = Vec::new();
    for line in stderr.lines() {
        let Some(rest) = line.strip_prefix(file).and_then(|rest| rest.strip_prefix(':')) else {
            continue;
        };
        let mut parts = rest.splitn(4, ':');
        let (Some(line_number), Some(_), Some(level), Some(message)) =
            (parts.next(), parts.next(), parts.next(), parts.next())
        else {
            continue;
        };
        let level = level.trim().split('[').next().unwrap_or_default().to_owned();
        actual.push((line_number.parse().unwrap_or(0), level, message.trim().to_owned()));
    }
    actual
}

fn check(test: &Path, out_dir: &Path, externs: &[String]) -> bool {
    let file = test.to_str().expect("UTF-8 path");
    let source = std::fs::read_to_string(test).expect("read the test");
    let out_dir_arg = out_dir.to_str().expect("UTF-8 path");
    let mut args = vec![
        file,
        "--crate-type=lib",
        "--edition=2024",
        "--emit=metadata",
        "--error-format=short",
        "-L",
        out_dir_arg,
        "--out-dir",
        out_dir_arg,
    ];
    for extern_arg in externs {
        args.extend(["--extern", extern_arg.as_str()]);
    }
    let stderr = run_driver(&args);

    let mut unmatched = expected_diagnostics(&source);
    let mut unexpected = Vec::new();
    for (line, level, message) in actual_diagnostics(&stderr, file) {
        if level == "error" && message.starts_with("aborting due to") {
            continue;
        }
        match unmatched
            .iter()
            .position(|(l, lv, text)| *l == line && *lv == level && message.contains(text.as_str()))
        {
            Some(index) => {
                unmatched.remove(index);
            }
            None => unexpected.push((line, level, message)),
        }
    }

    for (line, level, message) in &unexpected {
        eprintln!("{file}:{line}: unexpected {level}: {message}");
    }
    for (line, level, text) in &unmatched {
        eprintln!("{file}:{line}: missing {level}: {text}");
    }
    let passed = unexpected.is_empty() && unmatched.is_empty();
    if !passed {
        eprintln!("--- driver output for {file}:\n{stderr}");
    }
    passed
}

fn main() -> ExitCode {
    let ui = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/ui");
    let out_dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("ui");
    std::fs::create_dir_all(&out_dir).expect("create the output directory");

    // Stand-ins for the crates whose items the lints know by path. The tests name libjs_rust's as `js`, as code outside
    // LibJS may not name the runtime's own modules (see Meta/Linters/check_libjs_rust_api.py).
    let mut externs = Vec::new();
    for (crate_name, alias, crate_type) in [
        ("libjs_rust", "js", "lib"),
        ("smallvec", "smallvec", "lib"),
        ("libjs_runtime_macros", "libjs_runtime_macros", "proc-macro"),
    ] {
        let source = ui.join(format!("auxiliary/{crate_name}.rs"));
        let library = match crate_type {
            "proc-macro" => out_dir.join(format!(
                "{}{crate_name}{}",
                std::env::consts::DLL_PREFIX,
                std::env::consts::DLL_SUFFIX
            )),
            _ => out_dir.join(format!("lib{crate_name}.rlib")),
        };
        let _ = std::fs::remove_file(&library);
        let mut args = vec![
            source.to_str().expect("UTF-8 path").to_owned(),
            format!("--crate-type={crate_type}"),
            format!("--crate-name={crate_name}"),
            "--edition=2024".to_owned(),
            "--out-dir".to_owned(),
            out_dir.to_str().expect("UTF-8 path").to_owned(),
        ];
        if crate_type == "proc-macro" {
            args.extend(["--extern".to_owned(), "proc_macro".to_owned()]);
        }
        let stderr = run_driver(&args.iter().map(String::as_str).collect::<Vec<_>>());
        if !library.exists() {
            eprintln!("building the {crate_name} stand-in failed:\n{stderr}");
            return ExitCode::FAILURE;
        }
        externs.push(format!("{alias}={}", library.display()));
    }

    let filter = std::env::args().nth(1);
    let mut tests: Vec<PathBuf> = std::fs::read_dir(&ui)
        .expect("list tests/ui")
        .map(|entry| entry.expect("directory entry").path())
        .filter(|path| path.extension().is_some_and(|extension| extension == "rs"))
        .filter(|path| {
            filter
                .as_ref()
                .is_none_or(|filter| path.to_string_lossy().contains(filter.as_str()))
        })
        .collect();
    tests.sort();

    let mut failures = 0;
    for test in &tests {
        let passed = check(test, &out_dir, &externs);
        println!("{} {}", if passed { "PASS" } else { "FAIL" }, test.display());
        failures += usize::from(!passed);
    }
    println!("{} passed, {failures} failed", tests.len() - failures);
    if failures == 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}
