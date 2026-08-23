#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Verify which Ladybird Web APIs can receive a SharedArrayBuffer.

This script outputs its results in six sections, numbered [1] through [6].

Sections [1] through [4] of the script's output report the results for checking these claims:

  A. Ladybird's WebIDL bindings generator implements the https://webidl.spec.whatwg.org/#js-arraybuffer buffer-source
     conversion steps, which say a SAB must be rejected with a TypeError unless the IDL type is annotated [AllowShared].
  B. So the set of entry points that can ever hand shared memory to an implementation is exactly the set that's
     annotated with [AllowShared]; everything else throws at the bindings layer — before any implementation code runs.

Rationale: If that holds, auditing "is our glue SharedArrayBuffer-safe?" only has to cover the [AllowShared] entry
points — not every API taking a BufferSource.

But, ES builtins have no bindings layer, and a Web API can take a buffer with no conversion running at all:
ReadableStreamDefaultController.enqueue() takes an "any", so an author can enqueue a view onto shared memory, and the
read loop will read it. Either way, the buffer gets there without passing any of the checks above.

So, sections [5] and [6] of the script's output report the results for checking these claims:

  C. Only two members of JS::ArrayBuffer hand out a pointer surface at all: data_at(), which points into the bytes, and
     shared_buffer(), which hands over the cross-process mapping for structured clone to transfer. Everything else that
     reads bytes copies them out. So, every place in the engine that could read shared memory while another agent writes
     it calls one of those two — and those places can be listed.
  D. That claim is only worthwhile if it stays true. So this script verifies that, by classifying every public member
     of ArrayBuffer against a table — and FAILS if it finds one it doesn't know. A new raw accessor will break the run.

Usage:  ./Meta/verify-sab-exposure.py [path-to-ladybird-checkout]
"""

import pathlib
import re
import sys
import time

from typing import NoReturn

# The generator's own buffer-source type list, from Meta/Generators/libweb_bindings/cpp_types.py (BUFFER_SOURCE_TYPES).
ARRAY_BUFFER_VIEW_TYPES = (
    "Int8Array",
    "Int16Array",
    "Int32Array",
    "Uint8Array",
    "Uint16Array",
    "Uint32Array",
    "Uint8ClampedArray",
    "BigInt64Array",
    "BigUint64Array",
    "Float16Array",
    "Float32Array",
    "Float64Array",
    "DataView",
)
# ...plus the IDL typedefs that expand to those unions.
BUFFER_SOURCE_NAMES = (
    ("ArrayBuffer",)
    + ARRAY_BUFFER_VIEW_TYPES
    + (
        "BufferSource",
        "ArrayBufferView",
        "AllowSharedBufferSource",
    )
)
INCLUDES_RE = re.compile(r"^\s*(\w+)\s+includes\s+(\w+)\s*;", re.MULTILINE)
BUFFER_SOURCE_RE = re.compile(r"\b(" + "|".join(BUFFER_SOURCE_NAMES) + r")\b")

# What the generator emits for a buffer-source parameter lacking [AllowShared].
# See Meta/Generators/libweb_bindings/to_idl_value.py, buffer_source_to_idl_value().
EMITTED_CHECK = "is_shared_array_buffer()"
GENERATOR_REL = "Meta/Generators/libweb_bindings/to_idl_value.py"


def classify_idl_occurrence(line):
    """Is this buffer-source occurrence a parameter, or a return type/attribute?

    Only a parameter converts an incoming JS value — so only a parameter can carry an SAB into an implementation. IDL
    has a regular-enough shape for a positional heuristic: Anything inside a method signature's parentheses is a
    parameter; anything else is a return type or an attribute.
    """
    if re.search(r"\battribute\b", line):
        return "attribute"
    open_paren = line.find("(")
    close_paren = line.rfind(")")
    if open_paren == -1 or close_paren < open_paren:
        return "attribute"
    inside = line[open_paren + 1 : close_paren]
    if BUFFER_SOURCE_RE.search(inside):
        return "parameter"
    return "return"


def fail(message) -> NoReturn:
    print(f"\n❌ FAILED: {message}", file=sys.stderr)
    sys.exit(1)


def find_generated_bindings(root):
    """Return (directory, cpp_files) for the most-recently generated bindings.

    Reports every candidate, because picking the wrong build directory silently produces a count of zero — which reads
    like "the check does not exist", rather than "you are looking at the wrong place".
    """
    candidates = []
    for path in sorted(root.glob("Build/*/Libraries/LibWeb/Bindings")):
        files = sorted(path.glob("*.cpp"))
        if files:
            newest = max(f.stat().st_mtime for f in files)
            candidates.append((newest, path, files))

    if not candidates:
        fail(
            "no generated bindings found under Build/*/Libraries/LibWeb/Bindings.\n"
            "            Build the project first (./Meta/ladybird.py build).\n"
            "            NOTE: an old build may hold a Build/*/Lagom/Libraries/LibWeb/Bindings\n"
            "            directory of *Constructor.cpp / *Prototype.cpp files. That's output from\n"
            "            a C++ generator removed in efe144552ce (2026-06-06); ignore it. Grepping\n"
            "            it finds no rejections at all — which reads like the check doesn't exist.\n"
        )

    print("Generated-bindings candidates:")
    for newest, path, files in candidates:
        stamp = time.strftime("%Y-%m-%d %H:%M", time.localtime(newest))
        print(f"  {len(files):5d} files  newest {stamp}  {path.relative_to(root)}")
    candidates.sort(key=lambda c: c[0], reverse=True)
    newest, path, files = candidates[0]
    print(f"  -> using {path.relative_to(root)}\n")
    return path, files, newest


def check_freshness(root, bindings_mtime):
    generator = root / GENERATOR_REL
    if not generator.exists():
        fail(f"generator not found at {GENERATOR_REL} — is this a Ladybird checkout?")
    if generator.stat().st_mtime > bindings_mtime:
        fail(
            f"{GENERATOR_REL} is NEWER than the generated bindings.\n"
            "            The bindings are stale; rebuild before trusting this analysis."
        )
    print(f"✅ Freshness: generated bindings are newer than {GENERATOR_REL}.\n")


# Every public member of JS::ArrayBuffer, and what it does with the bytes. The audit in [5] and [6] rests on this table
# being complete — so check_arraybuffer_surface() fails on anything not listed here.
ARRAYBUFFER_MEMBERS = {
    # Hands out a live pointer into the bytes. *The* hazard for shared memory: whatever the caller does with that
    # pointer, another agent may be writing underneath it.
    "data_at": "RAW",
    # Hands out the cross-process mapping itself — which is a pointer surface of its own (AnonymousBuffer exposes
    # data<T>()). Its purpose is transfer, not reading: structured clone passes the descriptor to the other agent so
    # both reference one [[ArrayBufferData]] — instead of copying the bytes.
    "shared_buffer": "RAW",
    # Copy the bytes out. Safe by construction: the caller works on a snapshot.
    "copy_to": "COPY",
    "copy_to_byte_buffer": "COPY",
    "copy_data_to": "COPY",
    # Snapshots for shared buffers; hands out a pointer only for unshared ones.
    "with_readonly_bytes": "COPY",
    # Writes into the buffer. Racy against a concurrent reader by nature — that's what writing to shared memory means —
    # so not a read-stability question.
    "overwrite": "WRITE",
    "move_data": "WRITE",
    # Single-element access, where the memory model's no-tear rules apply.
    "get_value": "ELEMENT",
    "set_value": "ELEMENT",
    "get_modify_set_value": "ELEMENT",
    # Move the whole block in or out. detach_and_take_data_block() detaches first, and an SAB can't be detached — so,
    # it's not a path to shared bytes.
    "detach_and_take_data_block": "BLOCK",
    "set_data_block": "BLOCK",
    # Metadata and lifecycle. No byte access.
    "create": "META",
    "byte_length": "META",
    "external_memory_size": "META",
    "is_external": "META",
    "is_caged": "META",
    "shares_storage_with": "META",
    # An opaque id naming the shared object, so two buffers over one object can be told apart from two over different
    # ones. Note the contrast with shared_buffer above, which is RAW: that hands out the mapping, this hands out a
    # number that says nothing about where the bytes live and cannot be turned back into them.
    "shared_object_id": "META",
    "data_offset": "META",
    "max_byte_length": "META",
    "set_max_byte_length": "META",
    "did_change_data_block_capacity": "META",
    "try_resize": "META",
    "try_ensure_capacity": "META",
    "detach_key": "META",
    "set_detach_key": "META",
    "detach_buffer": "META",
    "register_cached_typed_array_view": "META",
    "is_detached": "META",
    "is_fixed_length": "META",
    "can_cache_typed_array_view_data_offset": "META",
    "is_shared_array_buffer": "META",
}

# Language constructs that can precede the declared name in a signature (a decltype(auto) return type, a cast).
# Deliberately only real C++ keywords: putting a plausible member name here would silently shrink the audit — which is
# the failure check_arraybuffer_surface() exists to prevent.
CPP_CONSTRUCTS = {
    "decltype",
    "sizeof",
    "alignof",
    "static_cast",
    "const_cast",
    "reinterpret_cast",
    "dynamic_cast",
    "noexcept",
}

# What correct handling of the shared case looks like in each element-access primitive. They don't all do it the same
# way — and requiring one shape of evidence from all three would report correct code as broken: get_value()/set_value()
# choose between an atomic access and a plain byte copy, so they have to ask whether the buffer is shared.
# get_modify_set_value() never asks — it always hands the operation a span aliasing the live bytes, and the operation
# itself performs the atomic read-modify-write. What would break it is the opposite mistake: handing over a copy, so an
# Atomics.* RMW would silently run on private memory.
ELEMENT_PRIMITIVE_EVIDENCE = {
    "get_value": {
        "missing": "never asks whether the buffer is shared, so it can't be taking an atomic load",
        "require": "is_shared_array_buffer()",
        "how": "branches on shared, for an atomic load",
    },
    "set_value": {
        "missing": "never asks whether the buffer is shared, so it can't be taking an atomic store",
        "require": "is_shared_array_buffer()",
        "how": "branches on shared, for an atomic store",
    },
    "get_modify_set_value": {
        "missing": "does not reach the live bytes, so a shared RMW would be lost",
        "require": "data_at(",
        "forbid": "copy_to_byte_buffer",
        "if_forbidden": "run the RMW on a copy",
        "how": "hands the operation the live bytes, so its atomic RMW reaches shared memory",
    },
}

# The TypedArray wrapper each element-access primitive is reached through. Their names are unambiguous — unlike the
# primitives' own.
ENTRY_POINT_FOR_PRIMITIVE = {
    "get_value": "get_value_from_buffer",
    "set_value": "set_value_in_buffer",
    "get_modify_set_value": "get_modify_set_value_in_buffer",
}

ARRAYBUFFER_HEADER = "Libraries/LibJS/Runtime/ArrayBuffer.h"


def check_arraybuffer_surface(root):
    """Confirm no member beyond the known ones hands out a live pointer.

    The output of [5] and [6] enumerates callers of the raw accessors. That's only an audit if the set of raw accessors
    is itself complete. So, parse the class — and fail on any member this script hasn't been taught about.
    """
    header = root / ARRAYBUFFER_HEADER
    if not header.exists():
        fail(f"{ARRAYBUFFER_HEADER} not found.")
    text = header.read_text(encoding="utf-8").splitlines()

    start = next((n for n, line in enumerate(text) if line.startswith("class JS_API ArrayBuffer final")), None)
    if start is None:
        fail("could not find 'class JS_API ArrayBuffer final' — the class was renamed?")
    public_body, seen_public = [], False
    for line in text[start:]:
        stripped = line.strip()
        if stripped == "public:":
            seen_public = True
            continue
        if line.startswith("};"):
            break
        # Keep going past private/protected sections, instead of stopping. There's no later public block now, but if we
        # added one, stopping here would silently drop it from both the member classification below and the caller audit.
        if stripped in ("private:", "protected:"):
            seen_public = False
            continue
        if seen_public:
            public_body.append(line)

    # Member declarations: at one indent level inside the class, the declared name is the first identifier followed by
    # '(' before the body brace. Anything after '{' is an inline body — calls made there aren't members of this class.
    declared = set()
    for line in public_body:
        if not re.match(r"^    [A-Za-z_~\[]", line):
            continue
        signature = line.split("{", 1)[0]
        for name in re.findall(r"\b([a-z_][a-z0-9_]*)\s*\(", signature):
            if name in CPP_CONSTRUCTS:
                continue  # a return type such as decltype(auto), a cast, ...
            declared.add(name)
            break  # the declared name; the rest of the signature is parameters

    unknown = sorted(n for n in declared if n not in ARRAYBUFFER_MEMBERS)
    if unknown:
        fail(
            "JS::ArrayBuffer has public member(s) this script does not know about:\n"
            + "".join(f"              {n}()\n" for n in unknown)
            + "            Classify each in ARRAYBUFFER_MEMBERS (RAW if it hands out a pointer into\n"
            "            the bytes) before trusting the audit below."
        )

    raw = sorted(n for n, kind in ARRAYBUFFER_MEMBERS.items() if kind == "RAW")
    classified = len(declared)
    print()
    members = ", ".join(n + "()" for n in raw)
    print(f"✅ Raw-access surface: JS::ArrayBuffer's only pointer-returning member(s): {members}")
    print(
        f"   (all {classified} public members classified; m_data_block is private, so\n"
        "   DataBlock's own data() isn't reachable from outside the class)\n"
    )
    return raw


def audit_raw_byte_access(root, enforcing_stems, allow_shared_stems):
    """--- Section [5]. List every caller of the raw accessors, split by whether a bindings gate protects it. ---"""
    raw = check_arraybuffer_surface(root)
    pattern = re.compile(r"\b(" + "|".join(raw) + r")\s*\(")

    hits = []
    for path in sorted((root / "Libraries").rglob("*.cpp")) + sorted((root / "Libraries").rglob("*.h")):
        # Match the one header by path, not by name: skipping every file called ArrayBuffer.h would drop a second
        # one's call sites out of this section silently, which is exactly the completeness this audit claims.
        if path.relative_to(root).as_posix() == ARRAYBUFFER_HEADER:
            continue  # the definitions themselves
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if pattern.search(line) and "PrimitiveStorage" not in line:
                hits.append((path.relative_to(root), n, line.strip()))

    print("-" * 72)
    print(f"[5] Every caller of the raw accessor(s): {len(hits)}")
    print("-" * 72)
    print("LibWeb callers are reachable with shared memory only through the [AllowShared]")
    print("entry points in [1]. LibJS callers have no bindings gate at all.\n")
    ungated = []
    for lib in ("LibJS", "LibWeb"):
        group = [h for h in hits if str(h[0]).startswith(f"Libraries/{lib}/")]
        print(f"  {lib}: {len(group)} call site(s)")
        for path, n, line in group:
            # Cross-reference: does this interface's own binding reject an SAB? If it does, the only shared bytes that
            # can reach here came through an [AllowShared] parameter. If it doesn't, nothing stopped one - either the
            # interface takes no buffer-source parameter at all (so the buffer arrived some other way, e.g. an 'any'
            # chunk from a stream), or it opts in.
            if lib == "LibWeb":
                if path.stem in allow_shared_stems:
                    note = "[✅ AllowShared: shared memory reaches here by design]"
                elif path.stem in enforcing_stems:
                    note = "[✅ binding rejects SharedArrayBuffer]"
                else:
                    note = "[⚠️ no rejection emitted for this interface — how does the buffer arrive?]"
                    ungated.append((path, n, line))
            else:
                note = "[ℹ️ no bindings layer]"
            print(f"    {path}:{n}  {note}")
            print(f"      {line[:96]}")
        print()

    if ungated:
        print(f"⚠️ {len(ungated)} LibWeb call site(s) in an interface whose binding emits no rejection.")
        print("   That's not automatically a bug; the interface *may* take no buffer-source parameter — such")
        print("   that nothing needed rejecting. But it *does* mean the bindings aren't what keeps shared")
        print("   memory away from this code. So, to confirm where each buffer comes from, check this code:")
        print()
        for path, n, _ in ungated:
            print(f"       {path}:{n}")
        print()
    else:
        print("✅ Every LibWeb call site sits in an interface whose binding rejects a")
        print("   SharedArrayBuffer. The LibJS ones have no bindings layer by nature.")
        print()

    other = [h for h in hits if not str(h[0]).startswith(("Libraries/LibJS/", "Libraries/LibWeb/"))]
    if other:
        print(f"   Other libraries: {len(other)} call site(s)")
        for path, n, line in other:
            print(f"    {path}:{n}")
            print(f"      {line[:96]}")
        print()

    return ungated


def audit_element_access(root):
    """--- Section [6]. Check that every primitive doing element access handles the shared case. ---

    The previous section covered code that holds a live pointer into the bytes. This one covers a different hazard: The
    element-access primitives read/write in place, and the memory model has rules about how they must do that for shared
    memory (IsNoTearConfiguration, and the atomicity Atomics.* depends on). They don't all satisfy those rules the same
    way — so a single blanket test would misjudge them. See ELEMENT_PRIMITIVE_EVIDENCE for what each one must show.
    """
    header = (root / ARRAYBUFFER_HEADER).read_text(encoding="utf-8")
    element_primitives = [n for n, kind in ARRAYBUFFER_MEMBERS.items() if kind == "ELEMENT"]

    print("-" * 72)
    print("[6] Element-access primitives: is the shared case handled?")
    print("-" * 72)

    unhandled = []
    for name in sorted(element_primitives):
        evidence = ELEMENT_PRIMITIVE_EVIDENCE.get(name)
        if evidence is None:
            fail(
                f"no shared-memory evidence defined for {name}().\n"
                "            Add it to ELEMENT_PRIMITIVE_EVIDENCE: a primitive nobody has said how to\n"
                "            check is a primitive this audit can't vouch for."
            )
        # The out-of-line definition, from its signature to the closing brace at column 0.
        match = re.search(rf"^\w[^\n]*ArrayBuffer::{name}\(.*?^\}}", header, re.DOTALL | re.MULTILINE)
        if not match:
            fail(
                f"could not find the definition of ArrayBuffer::{name}() in {ARRAYBUFFER_HEADER}.\n"
                "            It was renamed, or moved out of the header. Skipping it would let this audit\n"
                "            report success without ever checking that primitive."
            )
        body = match.group(0)

        problem = None
        if evidence["require"] not in body:
            problem = f"⚠️ {evidence['missing']}"
        elif evidence.get("forbid") and evidence["forbid"] in body:
            problem = f"⚠️ {evidence['forbid']} in the body — it would {evidence['if_forbidden']}"

        print(f"  {name:<24} {problem if problem else '✅ ' + evidence['how']}")
        for line in body.splitlines():
            if re.search(r"FIXME|TODO", line) and "shared" in line.lower():
                print(f"      {line.strip()[:92]}")
        if problem:
            unhandled.append(name)

    print()
    if unhandled:
        print(f"⚠️ {len(unhandled)} primitive(s) don't show what correct handling of shared memory")
        print("  looks like for them. Trace their callers:")
        for name in unhandled:
            # Search the TypedArray wrapper that fronts each primitive — not the primitive's own name: set_value() and
            # friends are common method names, and matching them directly buries the answer in dozens of unrelated
            # LibWeb classes.
            entry_point = ENTRY_POINT_FOR_PRIMITIVE.get(name)
            if entry_point is None:
                print(f"       {name}() <- no entry point mapped")
                continue
            callers = set()
            for path in sorted((root / "Libraries").rglob("*.cpp")):
                if re.search(rf"\b{entry_point}\s*\(", path.read_text(encoding="utf-8", errors="replace")):
                    callers.add(str(path.relative_to(root)))
            print(f"       {name}(), via {entry_point}()")
            for caller in sorted(callers):
                print(f"         {caller}")
    else:
        print("✅ Every element-access primitive handles shared memory the way its own design requires.")
    return unhandled


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    if not (root / "Meta" / "Generators" / "libweb_bindings").is_dir():
        fail(f"{root} does not look like a Ladybird checkout.")

    print("=" * 72)
    print("Which Ladybird Web APIs can receive a SharedArrayBuffer?")
    print("=" * 72)
    print(f"Checkout: {root}\n")
    print("Indicators: ✅ checked and holds        ⚠️ needs attention")
    print("            ℹ️ nothing here to check    ❌ this run proves nothing")
    print()

    bindings_dir, generated, bindings_mtime = find_generated_bindings(root)
    check_freshness(root, bindings_mtime)

    idl_files = sorted((root / "Libraries" / "LibWeb").rglob("*.idl"))

    # --- Section [1]. Entry points that OPT IN to receiving shared memory. ---
    allow_shared = {}
    for idl in idl_files:
        hits = [
            (n, line.strip())
            for n, line in enumerate(idl.read_text(encoding="utf-8").splitlines(), 1)
            if "[AllowShared]" in line
        ]
        if hits:
            allow_shared[idl] = hits

    print("-" * 72)
    print(f"[1] IDL files opting in with [AllowShared]: {len(allow_shared)}")
    print("-" * 72)
    for idl, hits in sorted(allow_shared.items()):
        print(f"\n  {idl.relative_to(root)}  ({len(hits)} declaration(s))")
        for n, line in hits:
            print(f"    :{n}  {line[:110]}")

    # --- Section [2]. Generated bindings that ENFORCE the rejection. ---
    enforcing = [f for f in generated if EMITTED_CHECK in f.read_text(encoding="utf-8")]
    print("\n" + "-" * 72)
    print(f"[2] Generated bindings emitting the rejection: {len(enforcing)} of {len(generated)}")
    print("-" * 72)
    for f in enforcing:
        print(f"  {f.relative_to(root)}")

    # --- Section [3]. Cross-check: every IDL using buffer sources is accounted for. ---
    #
    # Only an *incoming parameter* converts a JS value — so, only a parameter can carry an SAB into an implementation. A
    # buffer-source used as a return type or a readonly attribute is produced by us — never converted from script — so,
    # it needs no rejection. Classify each occurrence to prove that — rather than leaving it to the reader.
    print("\n" + "-" * 72)
    print("[3] Cross-check: IDL declaring buffer-source types, with no rejection")
    print("    emitted and no [AllowShared] opt-in")
    print("-" * 72)
    enforcing_stems = {f.stem for f in enforcing}
    # "Interface includes Mixin;" - map each mixin to the interfaces carrying its bindings.
    mixin_includers = {}
    for idl in idl_files:
        for interface, mixin in INCLUDES_RE.findall(idl.read_text(encoding="utf-8")):
            mixin_includers.setdefault(mixin, set()).add(interface)
    unaccounted = []
    for idl in idl_files:
        # Drop comments and whole typedef statements. A typedef declares the buffer-source union itself (BufferSource,
        # ArrayBufferView); it's not a use of one, and it can span several lines.
        body = []
        in_typedef = False
        pending = ""
        for line in idl.read_text(encoding="utf-8").splitlines():
            stripped = line.lstrip()
            if in_typedef:
                in_typedef = ";" not in line
                continue
            if stripped.startswith("typedef"):
                in_typedef = ";" not in line
                continue
            if stripped.startswith("//"):
                continue
            # An IDL declaration can span physical lines, and classification is positional: it looks for the buffer
            # source between a signature's parentheses. Given one line of a split declaration it sees no "(" at all
            # and calls the occurrence an attribute - so an ungated parameter on a continuation line would never
            # reach parameter_uses, and this section would report no gaps while one sat in front of it. Join the
            # declaration back together first. A statement ends at ";", and "{" or "}" ends whatever preceded it.
            pending = f"{pending} {stripped}".strip() if pending else stripped
            if ";" not in stripped and not stripped.endswith("{") and not stripped.startswith("}"):
                continue
            body.append(pending)
            pending = ""
        if pending:
            body.append(pending)
        hits = [line.strip() for line in body if BUFFER_SOURCE_RE.search(line)]
        if not hits:
            continue
        # A mixin's operations are generated into the bindings of every interface that includes it, not into its own
        # stub binding - so its stem never matches an enforcing one. Resolve through the includes graph, or every
        # declaration in a mixin looks ungated no matter what the generator emitted for it.
        if ({idl.stem} | mixin_includers.get(idl.stem, set())) & enforcing_stems:
            continue
        # [AllowShared] is an annotation on a declaration, not on a file. Skipping the whole file at the first
        # opt-in hides any ungated declaration beside it - which is exactly what this section exists to list. So
        # drop the opted-in declarations and keep the rest.
        hits = [line for line in hits if "[AllowShared]" not in line]
        if not hits:
            continue
        unaccounted.append((idl, hits))

    parameter_uses = []
    for idl, hits in unaccounted:
        for line in hits:
            kind = classify_idl_occurrence(line)
            if kind == "parameter":
                parameter_uses.append((idl, line))

    for idl, hits in unaccounted:
        print(f"\n  {idl.relative_to(root)}")
        for line in hits:
            print(f"    [{classify_idl_occurrence(line):>9}]  {line[:92]}")

    print()
    if parameter_uses:
        print(f"⚠️ {len(parameter_uses)} PARAMETER use(s) with no rejection and no opt-in.")
        print("  These would be real gaps: a SharedArrayBuffer could reach the")
        print("  implementation without the bindings rejecting it. Investigate:")
        for idl, line in parameter_uses:
            print(f"       {idl.relative_to(root)}: {line[:80]}")
    else:
        print("✅ All occurrences are return types or attributes — values we produce,")
        print("   never converted from an incoming JS value. No gaps.")

    # --- Section [4]. Negative control. ---
    print("\n" + "-" * 72)
    print("[4] Negative control: an API that does *not* opt in must reject")
    print("-" * 72)
    control = bindings_dir / "Crypto.cpp"
    control_failed = False
    if not control.exists():
        # A control that did not run is not a control that passed. Without it, nothing above is established: a run
        # that cannot show the generator still rejects an un-opted-in API says nothing about the ones it calls gated.
        control_failed = True
        print(f"⚠️ skipped: {control.relative_to(root)} is not present, so the control did not run.")
    else:
        lines = control.read_text(encoding="utf-8").splitlines()
        hit = next((n for n, line in enumerate(lines, 1) if EMITTED_CHECK in line), None)
        if hit is None:
            control_failed = True
            print(f"❌ {control.relative_to(root)} emits no rejection anywhere.")
            print("  getRandomValues() takes a BufferSource with no [AllowShared], so the")
            print("  generator should have gated it. Either it has stopped honoring the")
            print("  annotation, or these bindings are stale. Nothing above can be trusted.")
        else:
            print(f"✅ {control.relative_to(root)}:{hit}")
            for ctx in lines[max(0, hit - 3) : hit + 1]:
                print(f"    {ctx.strip()[:104]}")

    ungated = audit_raw_byte_access(root, {f.stem for f in enforcing}, {i.stem for i in allow_shared})
    unhandled = audit_element_access(root)

    print("\n" + "=" * 72)
    print("CONCLUSION")
    print("=" * 72)
    print(f"{len(enforcing)} of {len(generated)} generated bindings reject a SharedArrayBuffer.")
    print(f"{len(allow_shared)} IDL file(s) opt in via [AllowShared] — the only Web API")
    print("entry points through which shared memory can reach an implementation.")
    print()
    flagged = len(parameter_uses) + len(ungated) + len(unhandled)
    if control_failed:
        print("❌ The negative control did not pass — so, treat everything above as unverified.")
    elif flagged:
        print(f"⚠️ {flagged} item(s) above need attention; search this output for ⚠️ (needs-attention) markers.")
    else:
        print("✅ Nothing above needs fixing: every buffer-source parameter is gated, every")
        print("  raw-access caller is accounted for, and every element-access primitive")
        print("  handles shared memory the way its own design requires.")


if __name__ == "__main__":
    main()
