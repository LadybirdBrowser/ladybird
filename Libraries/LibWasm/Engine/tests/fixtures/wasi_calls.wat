;; WASI functions, exported as they are, for tests to call with arguments of their choice.
(module
  (import "wasi_snapshot_preview1" "fd_close" (func $fd_close (param i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write" (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_open"
    (func $path_open (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_create_directory" (func $path_create_directory (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_filestat_get"
    (func $path_filestat_get (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (export "fd_close" (func $fd_close))
  (export "fd_write" (func $fd_write))
  (export "path_open" (func $path_open))
  (export "path_create_directory" (func $path_create_directory))
  (export "path_filestat_get" (func $path_filestat_get)))
