/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WASI's descriptors and its capability model, through the functions a guest imports.

#![cfg(unix)]

use libwasm_engine::runtime::{ExternValue, InstanceAddress, Store, Value};
use libwasm_engine::wasi::{MODULE_NAME, Preopen, Wasi};
use std::rc::Rc;
use std::sync::Arc;

const ERRNO_SUCCESS: i32 = 0;
const ERRNO_BADF: i32 = 8;

struct Guest {
    store: Store,
    instance: InstanceAddress,
}

impl Guest {
    fn new(preopens: Vec<Preopen>) -> Self {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/wasi_calls.wasm");
        let module = Arc::new(libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap());
        let mut store = Store::new();
        let wasi = Rc::new(Wasi::new(Vec::new(), Vec::new(), preopens));
        let mut imports = Vec::new();
        for import in &module.imports {
            assert_eq!(import.module, MODULE_NAME);
            let function = wasi.allocate_function(&mut store, &import.name).unwrap();
            imports.push(ExternValue::Function(function));
        }
        let instance = store.instantiate(module, &imports).unwrap();
        let Some(ExternValue::Memory(memory)) = store.instance(instance).export("memory") else {
            panic!("no memory export");
        };
        wasi.set_memory(memory);
        Self { store, instance }
    }

    fn call(&mut self, name: &str, arguments: &[Value]) -> i32 {
        let Some(ExternValue::Function(function)) = self.store.instance(self.instance).export(name) else {
            panic!("no function export {name}");
        };
        match self.store.invoke(function, arguments).unwrap()[..] {
            [Value::I32(errno)] => errno,
            ref other => panic!("{name} returned {other:?}"),
        }
    }
}

#[test]
fn closing_standard_streams_keeps_the_hosts_open() {
    let mut guest = Guest::new(Vec::new());
    assert_eq!(guest.call("fd_close", &[Value::I32(1)]), ERRNO_SUCCESS);
    // The guest's descriptor is gone, the host's standard output is not.
    assert_eq!(guest.call("fd_close", &[Value::I32(1)]), ERRNO_BADF);
    assert_eq!(
        guest.call(
            "fd_write",
            &[Value::I32(1), Value::I32(0), Value::I32(0), Value::I32(64)]
        ),
        ERRNO_BADF
    );
    assert!(unsafe { libc::fcntl(1, libc::F_GETFD) } >= 0);
}

const ERRNO_LOOP: i32 = 32;
const ERRNO_NOTCAPABLE: i32 = 76;
const LOOKUP_SYMLINK_FOLLOW: i32 = 1;
const OFLAGS_CREAT: i32 = 1;
const RIGHTS_FD_READ: i64 = 1 << 1;

/// A directory tree for sandbox tests, removed afterwards:
///   root/secret.txt
///   root/sandbox/inside.txt, sub/, link_in -> inside.txt, link_out -> ../secret.txt, link_absolute -> <secret.txt>,
///   sub/link_up -> ../inside.txt, sub/link_out -> ../../secret.txt
struct Tree(std::path::PathBuf);

impl Tree {
    fn new(name: &str) -> Self {
        let root = std::env::temp_dir().join(format!("libwasm-wasi-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let sandbox = root.join("sandbox");
        std::fs::create_dir_all(sandbox.join("sub")).unwrap();
        std::fs::write(root.join("secret.txt"), "secret").unwrap();
        std::fs::write(sandbox.join("inside.txt"), "inside").unwrap();
        std::os::unix::fs::symlink("inside.txt", sandbox.join("link_in")).unwrap();
        std::os::unix::fs::symlink("../secret.txt", sandbox.join("link_out")).unwrap();
        std::os::unix::fs::symlink(root.join("secret.txt"), sandbox.join("link_absolute")).unwrap();
        std::os::unix::fs::symlink("../inside.txt", sandbox.join("sub/link_up")).unwrap();
        std::os::unix::fs::symlink("../../secret.txt", sandbox.join("sub/link_out")).unwrap();
        Self(root)
    }

    fn guest(&self) -> Guest {
        let sandbox = self.0.join("sandbox").to_str().unwrap().to_string();
        Guest::new(vec![Preopen::new(sandbox, "/sandbox".to_string())])
    }
}

impl Drop for Tree {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

impl Guest {
    /// Writes `path` at address 16, and returns its length.
    fn path(&mut self, path: &str) -> i32 {
        let Some(ExternValue::Memory(memory)) = self.store.instance(self.instance).export("memory") else {
            panic!("no memory export");
        };
        self.store.memory_mut(memory).bytes_mut()[16..16 + path.len()].copy_from_slice(path.as_bytes());
        path.len() as i32
    }

    fn open(&mut self, fd: i32, path: &str, follow: bool, oflags: i32) -> i32 {
        let length = self.path(path);
        let lookup = if follow { LOOKUP_SYMLINK_FOLLOW } else { 0 };
        self.call(
            "path_open",
            &[
                Value::I32(fd),
                Value::I32(lookup),
                Value::I32(16),
                Value::I32(length),
                Value::I32(oflags),
                Value::I64(RIGHTS_FD_READ),
                Value::I64(0),
                Value::I32(0),
                Value::I32(0),
            ],
        )
    }
}

#[test]
fn paths_stay_beneath_their_preopen() {
    let tree = Tree::new("open");
    let mut guest = tree.guest();
    for path in [
        "inside.txt",
        "./inside.txt",
        "sub/../inside.txt",
        "sub/",
        "sub/link_up",
        "link_in",
    ] {
        assert_eq!(guest.open(3, path, true, 0), ERRNO_SUCCESS, "{path}");
    }
    for path in [
        "../secret.txt",
        "sub/../../secret.txt",
        "..",
        "/etc/passwd",
        &tree.0.join("secret.txt").to_string_lossy(),
        "link_out",
        "link_absolute",
        "sub/link_out",
        "link_out/../sandbox/inside.txt",
    ] {
        assert_eq!(guest.open(3, path, true, 0), ERRNO_NOTCAPABLE, "{path}");
    }
    // Without following, a symbolic link in the last place is not opened.
    assert_eq!(guest.open(3, "link_out", false, 0), ERRNO_LOOP);
    // A link in the middle is followed, but not out.
    assert_eq!(guest.open(3, "link_out/x", false, 0), ERRNO_NOTCAPABLE);
    // Nothing is created outside either.
    assert_eq!(guest.open(3, "../created.txt", false, OFLAGS_CREAT), ERRNO_NOTCAPABLE);
    assert!(!tree.0.join("created.txt").exists());
}

#[test]
fn descriptors_the_guest_does_not_have_resolve_nothing() {
    let tree = Tree::new("badf");
    let mut guest = tree.guest();
    // These resolved against the host's working directory before.
    assert_eq!(guest.open(99, "inside.txt", true, 0), ERRNO_BADF);
    let length = guest.path("created");
    assert_eq!(
        guest.call(
            "path_create_directory",
            &[Value::I32(99), Value::I32(16), Value::I32(length)]
        ),
        ERRNO_BADF
    );
}

#[test]
fn directories_and_status_stay_beneath_their_preopen() {
    let tree = Tree::new("other");
    let mut guest = tree.guest();
    let length = guest.path("../escaped");
    assert_eq!(
        guest.call(
            "path_create_directory",
            &[Value::I32(3), Value::I32(16), Value::I32(length)]
        ),
        ERRNO_NOTCAPABLE
    );
    assert!(!tree.0.join("escaped").exists());
    let length = guest.path("sub/made");
    assert_eq!(
        guest.call(
            "path_create_directory",
            &[Value::I32(3), Value::I32(16), Value::I32(length)]
        ),
        ERRNO_SUCCESS
    );
    assert!(tree.0.join("sandbox/sub/made").is_dir());
    for (path, follow, errno) in [
        ("inside.txt", true, ERRNO_SUCCESS),
        ("link_in", true, ERRNO_SUCCESS),
        ("link_out", false, ERRNO_SUCCESS),
        ("link_out", true, ERRNO_NOTCAPABLE),
        ("../secret.txt", false, ERRNO_NOTCAPABLE),
    ] {
        let length = guest.path(path);
        let lookup = if follow { LOOKUP_SYMLINK_FOLLOW } else { 0 };
        let arguments = [
            Value::I32(3),
            Value::I32(lookup),
            Value::I32(16),
            Value::I32(length),
            Value::I32(1024),
        ];
        assert_eq!(guest.call("path_filestat_get", &arguments), errno, "{path}");
    }
}

const ERRNO_ISDIR: i32 = 31;
const ERRNO_NOTDIR: i32 = 54;

#[test]
fn trailing_slashes_name_only_directories() {
    let tree = Tree::new("slash");
    let mut guest = tree.guest();
    assert_eq!(guest.open(3, "sub/", false, 0), ERRNO_SUCCESS);
    assert_eq!(guest.open(3, "inside.txt/", false, 0), ERRNO_NOTDIR);
    // A trailing slash follows the link in the last place, beneath the preopen only.
    assert_eq!(guest.open(3, "link_in/", false, 0), ERRNO_NOTDIR);
    assert_eq!(guest.open(3, "link_out/", false, 0), ERRNO_NOTCAPABLE);
    assert_eq!(guest.open(3, "created/", false, OFLAGS_CREAT), ERRNO_ISDIR);
    assert!(!tree.0.join("sandbox/created").exists());
    for (path, errno) in [
        ("sub/", ERRNO_SUCCESS),
        ("inside.txt/", ERRNO_NOTDIR),
        ("link_in/", ERRNO_NOTDIR),
    ] {
        let length = guest.path(path);
        let arguments = [
            Value::I32(3),
            Value::I32(0),
            Value::I32(16),
            Value::I32(length),
            Value::I32(1024),
        ];
        assert_eq!(guest.call("path_filestat_get", &arguments), errno, "{path}");
    }
    let length = guest.path("sub/made/");
    assert_eq!(
        guest.call(
            "path_create_directory",
            &[Value::I32(3), Value::I32(16), Value::I32(length)]
        ),
        ERRNO_SUCCESS
    );
    assert!(tree.0.join("sandbox/sub/made").is_dir());
}
