# Rust GC lints

A rustc driver with lints for the Rust crates that use libjs_rust's GC, the first two ported from the LibJSGC clang
plugin:

- `gc_container_on_stack` (`gc_pointer_container_on_stack` in C++): a local, parameter, return value or unsizing
  coercion whose type owns a GC pointer through heap memory that the collector does not scan (`Vec`, `VecDeque`,
  `HashMap`, `BTreeMap`, `Box`, `Rc`, `Arc`, `SmallVec`, a boxed closure that captures one, a struct holding any of
  these, ...). Keep such lists in a `MarkedVec`, or lend them from one.
- `root_in_traced_type` (`strong_root_fields_in_gc_allocated_types`): a field that holds a `Root` or `MarkedVec` in a
  type that implements `Trace`.
- `untraced_gc_pointer`: a field marked `#[gc(untraced)]`, which `#[derive(Trace)]` skips, whose type can hold a GC
  pointer. A field whose cells something else keeps alive says what with `#[gc(untraced = "...")]`.
- `gc_pointer_in_drop`: the `Drop` of a type that the crate's cells own, by value or through heap memory, dereferences a
  `Gc`. Cells are dropped while the collector sweeps, when the cells they point to may be gone already. What needs other
  cells belongs in `Finalize`, which runs while every cell that dies is intact.
- `gc_container_in_untraced_type`: a type that does not implement `Trace` keeps a GC pointer in a field through heap
  memory, an `Rc` or a `Box` included. Nothing traces its values wherever they are held: implement `Trace` for it, so
  that its owner traces it, or tie it to a deferral.

A GC pointer is a reference to a `GcCell`, a value of a type of the crate that implements `Trace` by hand, as `Gc`,
`Value` and `PropertyKey` do, or a trait object whose trait requires `Trace`. A type that hides cell pointers in its
bits has to trace them to keep their cells alive, so no type can hide them from the lints. What `#[derive(Trace)]`
implements is checked through the fields.

The check reads types after inference, so `let cells = make_cells();` is checked as `Vec<Gc<Cell>>`. Std containers
need no list: a container owns its storage through a `PhantomData<T>` next to its pointer, and the lint treats that `T`
as heap memory. `SmallVec` keeps its spill behind a bare pointer, so it is listed by path.

These pass:

- Types that implement the unsafe `KeepsAlive` marker: `Root` and `MarkedVec`, whose values are roots, `Deferred`,
  which cannot outlive the `DeferGc` it is tied to, and the `Vm`, which is the root set. References and slices borrow,
  and pass too.
- Types that implement the unsafe `SweptAfterCollection` marker, whose pointers do not keep cells alive, or the unsafe
  `HoldsNoCells` marker, like `NonSymbolPropertyKey`.
- The parameters, locals and return values of a function or closure that takes a `&DeferGc` (a type that implements
  the unsafe `DefersCollections` marker, as the `Sweep` of a collection's sweep callback does too), and of the closures
  in it, which nothing collects while they live. What such a function returns is checked where its caller holds it. A
  guard bound in a body covers nothing, as it can be dropped while what it should cover lives on.
- A struct with a field of a type that implements `DefersCollections`, a `&DeferGc` or a `Deferred`, which its values
  cannot outlive.

A cell exists by value only while `Heap::allocate()` builds it, in the closure it is given, which takes the deferral
that lasts until the cell is in the heap: C++ constructs cells in place the same way. The constructors of cells and of
their storage that hold GC pointers in heap memory take a `&DeferGc`, so that only such a builder can call them.

The lints find the types of the GC through the traits in `libjs_rust::gc::lint_markers` (and `Trace`, `GcCell` and
`SweptAfterCollection`), and stop with an error if libjs_rust no longer has one, so that moving a type does not hide it.

## Running

```bash
Meta/RustLints/run.sh
```

builds the driver on the nightly pinned in `rust-toolchain.toml` (it links that nightly's compiler, so it needs the
`rustc-dev` component) and runs `cargo check` on `libjs_rust`, `libweb_bindings_rust` and `libweb_rust` with the driver
as `RUSTC_WORKSPACE_WRAPPER`. Dependencies outside the workspace are compiled by the nightly's plain rustc. Set
`LADYBIRD_RUST_LINTS_WARN=1` to report hits as warnings, so crates that depend on one with hits are still checked, and
`LIBWEB_RUST_BINDINGS` to the `RustBindings.rs` of a build to check the generated bindings too.

The tests compile each `tests/ui/*.rs` against stand-ins for libjs_rust and smallvec and compare the diagnostics with
the `//~ ERROR` annotations:

```bash
cargo test --manifest-path Meta/RustLints/Cargo.toml
```

## Suppressing a hit

The lints exist only under the driver, so name them under the cfg it sets, with a reason:

```rust
#[cfg_attr(ladybird_gc_lints, expect(gc_container_on_stack, reason = "every cell is also in the realm's list"))]
let cells = ...;
```

An `allow` or `expect` without a reason is an error. Prefer `expect`, which warns once the hit is gone.
