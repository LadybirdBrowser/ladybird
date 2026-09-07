<!--
Copyright (c) 2026-present, the Ladybird developers.

SPDX-License-Identifier: BSD-2-Clause
-->

# Rust stylesheet ownership

Downloaded link and import sheets use
`Parser::parse_stylesheet_off_thread`. CSSOM parsing borrows native text synchronously.
Workers receive owned parsing context, never document or interner access.

`ParsedStyleSheet` is immutable and thread-shareable. Its weak parse cache is
keyed by native sheet text and parsing context. Parsed selectors, declarations,
descriptors and URL spelling use shared Rust storage. Parsing and destruction
on a worker must not call C++ or the main-thread `Utf16FlyString` interner.

`NativeRuleList` retains that graph and reserves stable identities without
creating mutable rule or declaration owners. Its visible-rule indexes support
indexed CSSOM access. A sparse map holds exposed rules; order-changing mutation
can materialize a list without recursively materializing descendants.
`RuleRef` traverses both exposed and untouched rules. Keyframes use the same
child list for compilation and CSSOM, with no parallel frame-owner list.

`rule/compilation.rs` binds names on the document thread and publishes directly
into the Rust style engine. Compiled targets retain immutable declarations,
source identities and condition inputs, not CSSOM objects. Cascading borrows
native stylesheet and inline declaration data.
Media evaluation and condition/layer traversal follow imports directly in Rust.
The C++ entry points borrow the media environment once; layer traversal reuses
one UTF-16 prefix buffer across the graph. Rust also merges cascade layer order
and publishes condition gates directly. Compilation, layer publication and
replay use native recorded operations; C adapters exist only for host callers.

`StyleSheetState` and `StyleSheetImport` own document/loading state independently
of the GC-allocated `CSSStyleSheet` and `CSSImportRule` facades. Imports retain
their loading media identity. Font, function, keyframe and property-registration
consumers read shared rule data without creating CSSOM wrappers.

CSSOM facades retain native rules and borrow their payloads where possible.
Live declaration and descriptor handles share mutation state; independent
sheets share immutable data until mutation. Explicit immutable snapshots
remain valid across mutation. C++ property caches needed by CSSOM/resource
consumers are populated directly, without an intermediate cached FFI array.

CSS URL spelling may contain Unicode. URL resolution is a later document-thread
operation, not worker stylesheet parsing. LibURL accepts borrowed ASCII/UTF-16,
and parsed URL components and base URL snapshots stay ASCII. The IDNA boundary
still calls C++/ICU. Rust and AK use the same C allocator for transferable buffers.

The Rust load-and-compile invariant covers worker parsing, imports, media,
namespaces, layers and definition readers. It requires zero C++ worker calls
and zero mutable rule, declaration or descriptor owner allocations for an
untouched sheet. HTML/JS tests cover mutation, sharing, imports and wrapper
identity. Keep those guarantees when simplifying this boundary.
