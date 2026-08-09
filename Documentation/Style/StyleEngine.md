# StyleEngine: differential style resolution

This document describes LibWeb's style engine: its data structures, algorithms, protocols, and memory rules, as implemented. Normative statements ("never", "must") are invariants the implementation upholds and that changes must preserve; they are review tests, not aspirations. Testing and debugging workflows live in [StyleEngineTesting.md](StyleEngineTesting.md).

`StyleEngine` is LibWeb's implementation of CSS selector evaluation, style invalidation, cascade, and computed-value computation. Mutations reach it as typed input deltas (§4.3), and compiled delta routing (§6.3) carries each delta to the transpose entry points that mention that input: every selector program that can change under the delta is reached, and subjects are match-checked before any truth delta is emitted. Selector answers, cascade winners, and computed styles are shared, interned values: elements hold a compact `StyleRecordID` into interned computed-value groups (§9.8), and every consumer of style (layout, paint, hit testing, accessibility, script APIs, and the rest of the inventory in §12) reads through that shared handle.

Correctness rests on the engine's internal fallback chain: incremental evaluation, then exact evaluation of the affected scope, widening to the whole document when required (§10.6). Every path reads the same authoritative live DOM, and the exact cold evaluator is the reference implementation the others are checked against; the machinery that continuously enforces this is described in [StyleEngineTesting.md](StyleEngineTesting.md).

One organizing frame runs through the whole document and is stated in §1.1: StyleEngine is a database, specifically an incremental view maintenance engine over a change stream. Mechanisms are described together with their database-world counterparts, in notes marked *Database counterpart*.

## 1. What the engine does

Conventional style engines do two things per change: conservatively determine which elements *may* be affected, then re-run selector matching, cascade, and computed-value construction for all of them. Work is proportional to the *possible* effects of a change rather than its *actual* semantic effects.

StyleEngine models styling as a memory-bounded incremental computation. DOM structure, element state, stylesheets, CSSOM mutations, cascade topology, and environment values are versioned inputs. Selectors, cascade decisions, and computed values form a shared logical dependency program. A style flush discovers and propagates exact changes through that program and stops as soon as a semantic value is unchanged.

The pipeline:

```text
Style input transaction
    -> tree, state, stylesheet, cascade, and environment deltas
    -> exact selector-truth deltas
    -> exact matched-rule deltas
    -> exact cascade-winner deltas
    -> exact computed-property deltas
    -> layout, paint, animation, and observable-style changes
```

Each arrow is a stopping point. If a stage's output identity is unchanged, the next stage receives nothing.

The cost of a transaction is:

```text
work = normalize + discover + propagate + reconstruct + observe
```

`discover` can dominate even when no semantic output changes, so it is accounted separately and the engine switches to an exact batch plan when a supposedly selective plan does too much discovery.

**The ordinary path is the primary kernel.** Tag, ID, class, attribute, state, compound, and conventional combinator evaluation must be the fastest and smallest path through the engine. `:has()` is a forcing function for the architecture, not the center of the workload: it extends the same selector primitives with reverse-direction existential maintenance, and it must impose no cost on documents that do not use it (zero relational allocations, zero relational fields).

### 1.1 The engine is a database

StyleEngine is a database engine: an incremental view maintenance engine over a change stream. The queries are standing rather than ad-hoc (attached selector programs change only when the program itself is edited), inputs arrive as typed deltas, and the engine's entire job is to keep a set of partially materialized views consistent with the base data for less than the cost of recomputing them. That places it in the family of streaming materialization systems (differential dataflow, DBSP, and the partially stateful Noria line), not general-purpose OLTP stores, and that family's known results, failure modes, and remedies apply here directly.

The canonical mapping, used throughout:

| StyleEngine mechanism | Database counterpart |
| --- | --- |
| Authoritative DOM, CSSOM, browser state (Tier 0) | Base tables, the single source of truth |
| Attached selector and cascade programs | Standing (continuous) queries |
| Ephemeral selector programs (§6.10) | Ad-hoc queries |
| Normalization journal (§4.4) | Changelog (CDC stream) with key-level compaction |
| Style read epoch (§4.2) | MVCC snapshot read view, without copying |
| Style transaction (§4.4, §11) | Transaction: atomic commit, isolated from script |
| Match program (§6.3) | Query plan: predicate evaluation per candidate row |
| Transpose program (§6.3) | Delta rule: the standing query's derivative |
| Delta-routing registry (§6.3) | Trigger dispatch index |
| Impact region (§6.4) | Delta key range; coverage is fail-closed (gaps widen) |
| Feature postings (§6.6) | Secondary (inverted) indexes, non-clustered |
| Rule matching (§6.8) | A relational join |
| Selective-plan cutoff (§6.7) | Cost-based optimizer: index scan versus full scan |
| Incremental discipline (§3) | Incremental view maintenance (fast refresh) |
| Exact batch plan (§3) | Complete refresh of the affected region; the upquery |
| Witnesses, winner groups, match answers | Partially materialized views |
| Tier-3 budget and eviction (§10) | Buffer pool with admission control |
| Cold-batch protocol (§13) | Storage scan API with projection and predicate pushdown |
| Cost model and counters (§15, §16) | Optimizer cost model and its statistics |

Three consequences of the framing carry weight throughout:

1. **Maintain-versus-refresh is a per-delta optimizer decision, not an architectural loyalty.** No mature system promises that incremental maintenance always wins; it prices the delta against the rescan and chooses. §6.7 is that decision for selector plans, and the same decision governs every retained view, fed by §16's counters. When applying a delta costs more than enumerating the affected scope, rescanning is the optimizer's correct output, not a failure of the design.
2. **Every view is partial.** Any materialization may be missing, and a miss is answered by an upquery to base facts through the exact cold evaluator, never by a wrong answer. That is why eviction is always legal (invariant 5), why memory never scales with the product of DOM and selector count (invariant 3), and why the eviction policy matters: a policy that starves the views whose maintenance would pay converts the engine into a full-scan engine with extra bookkeeping.
3. **Propagation stops at unchanged output.** Every stage boundary in the §1 pipeline is a distinctness check: a delta whose output identity is unchanged is consolidated away and the next stage receives nothing. That is streaming consolidation, and it is invariant 1.

Where the analogy stops: there is no durability and no recovery, which is why the journal is a changelog and not a write-ahead log (§4.4); the data model is an ordered tree whose axes (ancestry, sibling order, the flat tree) are join dimensions no relational engine indexes natively; and the query set is fixed at parse time, so plan compilation is a program-change event, not a per-query cost. Importing machinery for problems the engine does not have (locking, recovery logging, general ad-hoc planning) is a misreading of the framing.

Where a mechanism's counterpart has a known failure mode or remedy in the database literature, a short *Database counterpart* note names it; consult that literature before inventing a new remedy.

## 2. Invariants

These hold for every code path. They are not optional optimizations.

1. If a semantic value does not change, no downstream semantic work is emitted.
2. DOM mutations, state changes, CSSOM changes, and environment changes are equally first-class inputs.
3. Core persistent memory is proportional to DOM size, stylesheet program size, and live observable style state, never to the product of DOM size and selector count.
4. Selector matches, reverse dependencies, witnesses, proofs, and indexes are optional materialized views unless explicitly classified as core state (§10.1).
5. Every optional materialization can be discarded at any time without affecting correctness.
6. High fanout uses exact pull evaluation rather than millions of explicit dependency edges.
7. Cold, disconnected, hidden, and unobserved content does not receive per-selector state merely because it exists.
8. Every cache and arena reports exact byte usage and participates in the document budget.
9. Memory pressure reduces retained acceleration state, never correctness.
10. Interning, compaction, and physical relabeling never create semantic style changes.
11. The exact cold evaluator is part of the architecture. Optional incremental state may accelerate it but is never required for correctness.
12. Every selector dependency has a compiled delta-routing entry point. Routing semantic inputs to selector programs is required program state, not an evictable view.
13. Every scoped evaluation carries an impact-region coverage obligation (§6.4). Unknown scope is never treated as empty scope.
14. Style reads use the authoritative live DOM under a mutation-free read epoch. A complete copied DOM is never allocated.
15. Hot transpose and impact-region traversal has bounded evaluator-local access to every required tree relation, with no boundary crossing per relation step.
16. Every broad or degraded path is named, counted, and reachable in tests. Nothing silently degrades to full-document invalidation.

### 2.1 Explicitly rejected as primary architecture

Alternatives with known failure modes, and why they were rejected:

* **Full materialization of every view**: storing every selector state, match, witness, and reverse edge approaches the product of DOM and stylesheet size. The database framing of §1.1 survives this rejection precisely because its views are partial and evictable.
* **Conservative invalidation followed by restyle**: preserves the separation between possible effect and semantic change, and therefore preserves its work amplification.
* **Permanent proof subscriptions**: exact dependency edges are good for hot low-fanout queries, unacceptable as mandatory state for every match and negative result.
* **Rebuilding on CSSOM program change**: dynamic stylesheet insertion and editing are normal page behavior, especially during load; they use incremental query-program operations.
* **A complete computed-style object per element**: repeated style groups, inherited environments, and animation bases share storage through compact handles.

### 2.2 Engineering principles

These are the working principles the invariants compile down to in day-to-day changes. They are review tests, not aspirations.

**Work is mutation-sized; a whole-DOM traversal is a purchase, never a default.** A style node may be visited only when the mutation named it, a compiled route reaches it, or its own output changes. A whole-document pass is one of three things: output (that many styles changed), a purchase (one counted pass that provably beats the per-node work it replaces), or a named, counted correctness floor. The review test for every loop: name what bounds it; if the bound is the document and the output is not, the loop is a bug, even while it is fast. Visit a node at most once per flush across all stages, and never restart an interrupted pass; grow its window instead.

**Represent state data-oriented: dense tables over sparse maps, columns over objects.** A dense integer key (element index, atom, rule) indexes a vector or segmented column, never a hash map; hash maps are for content-keyed intern tables. Every dense per-node structure takes on the retirement story the day it is born, so a reused identity cannot inherit its predecessor's state. An evicted structure reads as unknown, never as empty.

**Choose presence structures by clearing cost.** A bitmap with a companion set-index list when the set lives for a transaction (cleared through the list, not memset); an epoch-stamped seen array when the set clears per candidate; a sorted vector when built once and only queried. A maintained population count is free exact cardinality where a plan cutoff wants one.

**Sort once at a build boundary; inherit order on the hot path.** Convert order labels to dense ordinals at program build and read integers after. A per-subject sort during evaluation is a smell: inherit order from pre-sorted program structures and merge. Sorting the changed set once per transaction is the right shape; sorting anything document-sized falls under the traversal principle.

**Spend document-order coordinates once they are bought.** With preorder positions and subtree ends, a subtree is a slice, ancestry an interval comparison, and skipping a subtree an index jump. The coordinates answer for one named tree relation and the current side only, and they are transaction scratch unless measurement justifies persistence.

## 3. Evaluation disciplines and plans

Two evaluation disciplines share one semantics:

**Incremental**: the normal discipline. Retained derived state (prefix states, retained answers, winner groups) and exact deltas minimize discovery and downstream work.

**Exact evaluation of a scope**: the engine ignores optional derived state and exactly evaluates an affected region with the exact evaluator against authoritative facts. It may inspect every style node in the region but emits only actual selector, rule, cascade, and computed-value changes. This is the correctness path and the universal response to eviction, memory pressure, missing state, or planner uncertainty. An exact batch is still an exact transaction: broad input scope, only changed outputs.

The choice is made per routed region, as a `Plan`:

```text
Selective   { candidates }   drive from an enumerable candidate source
ExactBatch  { nodes }        sweep the region with the exact evaluator
Conservative                 neither is executable; keep the routed region as-is
```

`Plan::Conservative` is not a restyle mode: it keeps the conservatively routed region, whose members are still checked by the match program before any delta is emitted, so precision is lost but correctness is not. There is no separate "recompute and republish" fallback mode; every degraded path narrows to one of the shapes above, and the broad ones are counted.

## 4. Inputs, epochs, and transactions

### 4.1 Terminology

* **Semantic input**: an authoritative fact from DOM, CSSOM, browser state, or environment.
* **Derived value**: a selector result, rule match, cascade winner, computed value, or damage result computed from semantic inputs.
* **Logical program**: the immutable, shared description of selector, activation, cascade, and property dependencies.
* **Materialized view**: a cached physical representation of a derived value or relation.
* **Style transaction**: a batch of input changes applied atomically from the perspective of style observation.
* **Style read epoch**: a mutation-free interval in which style reads the authoritative live DOM and other versioned inputs.
* **Style node**: the engine's compact identity for a DOM element.
* **Style feature atom**: a compact identity for a selector-mentioned tag, ID, class, attribute, value, namespace, or state fact. It does not own duplicate string payload.
* **Match program**: bytecode answering whether a candidate subject matches a selector during one read epoch.
* **Transpose program**: bytecode that starts from a changed selector input and traverses inverse selector relations to enumerate a safe superset of subjects whose match truth can change.
* **Delta-routing registry**: required program state mapping a typed semantic input key to transpose entry points.
* **Impact region**: the normalized union of subjects, relation ranges, and dependent scopes emitted by transpose programs for a transaction.
* **Impact-region attribution**: the split between regions attributed to a specific rule and program (eligible for exact planning) and unattributed regions (evaluated conservatively).
* **Match-answer identity**: a compact identity for one normalized exact selector answer.

### 4.2 Style read epoch

The epoch is a logical immutability contract, **not** a copied DOM or CSSOM snapshot, and not a reified object: it is upheld by phase rather than by a header. The evaluator runs synchronously at a point where script cannot mutate style-visible inputs, an FFI callback guard rejects reentrant entry into the engine, and identity retirement is deferred (retired `StyleNodeID`s are released for reuse only at a later safe boundary), so nothing an active evaluation can name is destroyed or reused under it. The evaluator queries current tree relations, element features, and document state from their authoritative live objects; nothing walks, clones, flattens, or retains the DOM.

The final live tree is the authoritative new state. The old side is not pinned: the normalization journal's first record per key preserves the pre-transaction value (§4.4), retained derived state supplies old selector truth where resident, and tree/program transactions reconstruct their before side from those sources when planning after commit. Compact mutation records carry only the old-side facts needed to discover everything whose output may have changed.

*Database counterpart:* the isolation is phase-based rather than MVCC: one writer-free interval per flush, with before-images carried in the changelog instead of retained versions.

### 4.3 Typed deltas

Input kinds (`InputKind`, transaction.rs):

```text
TreeRelations       parent/sibling/scope/slot/host relation changes
LocalFeature        tag, ID, class, attribute, namespace fact changes
State               element and document state facts (:hover, :checked, ...)
ElementDeclaration  element-owned declaration blocks
ElementStyleInput   edge-triggered per-element style actions (not retained fact state)
Program             stylesheet program changes
CascadeTopology     layer/order topology changes
Environment         media, viewport, preference, and other environment inputs
```

Animation state is not an input kind: an element whose animation-relevant inputs changed is reached through the kinds above, and animation sampling is an overlay on published style (§9.11), not a change stream into the engine.

Typing matters: treating every change as a generic version bump moves work from mutation time into an unnecessarily broad lazy validation later.

`ElementDeclaration` covers declarations sourced from one style node rather than a stylesheet rule: the `style` attribute, HTML presentational hints, and SVG presentation attributes such as `fill` and `stroke`. The authoritative HTML/SVG mapping produces an immutable old/new declaration block with its language-defined cascade placement; StyleEngine does not reinterpret attribute syntax.

**One mutation can publish several independent typed effects.** Changing an SVG `fill` attribute may change an attribute-selector feature, a presentation declaration, and an `attr()` input. Routing sends each effect to its own consumers; handling one never stands in for the others.

### 4.4 Transaction boundaries and the normalization journal

Script performs many mutations before style is observed. Record them and normalize at the transaction boundary:

* Adding then removing the same class produces no final class delta.
* Inserting then deleting an unobserved rule produces no selector or cascade work.
* Changing a declaration three times retains only the old and final declaration block versions.
* Inserting several sheets during parsing produces one ordered program update.
* Moving a subtree twice retains only its original and final relationships.

Operations whose Web-platform semantics force synchronous observation establish a boundary. Style reads (resolved-style queries, layout-dependent APIs where required) flush all earlier input changes before returning. Normalization never combines changes across a required observation boundary.

**Journal structure.** Pending changes go into a keyed normalization journal:

* DOM/state keys: style-node identity + typed fact or relation.
* Program keys: stable sheet/rule/declaration/attachment/topology identity + edited field.

The first record for a key preserves the pre-transaction value; later records replace only the pending final value; cancellation removes the key. Raw mutation count is therefore not the default work multiplier.

*Database counterpart:* the journal is the changelog, a CDC stream, and not a write-ahead log: it exists for change propagation, not durability or recovery, so WAL disciplines (idempotent replay, checkpointing) do not transfer. Key-level last-writer-wins is log compaction, the retained pre-transaction value is the before-image, and the overflow marker below is compaction of a key range into one coarse record whose replay is a scoped rescan.

For tree and feature mutations, the first record retains the minimum old-side information required by the applicable transpose rules:

* old and new feature atoms;
* old and new parent, adjacent-sibling, tree-scope, flat-tree, slot, host, and pseudo-relation endpoints;
* operator-specific boundary identities for affected sibling or subtree ranges.

A removed style-node identity and any required relation tombstone stay valid until the transaction retires. The journal does **not** copy unchanged descendant fields and does **not** preserve a traversable old DOM. Old-side facts exist to establish a complete impact region, not to reconstruct history.

Resident derived state supplies old selector truth when available. If it was evicted, discard the affected derived fragment, evaluate the final state from the live DOM, and compare with the last committed style output.

**Journal overflow.** The journal is byte-accounted against its own memory category. When growing it is refused, the most numerous input kind's fine-grained entries are replaced with a typed **complete-scope marker** covering the whole document for that kind, and later records of that kind are dropped outright. At flush a marker widens the impact region to the document, and the plan evaluates that region exactly (§3). A pathological script can force broad discovery work but cannot force unbounded journal memory. (Environment markers are exempted from disabling the retained-selector fast paths, since environment changes cannot alter selector incidence.)

### 4.5 Mutations during evaluation

During evaluation, DOM and CSSOM topology, element selector state, attached program order, and other style-visible inputs are logically immutable: the evaluator runs synchronously while the document cannot run mutating script, and the FFI boundary's callback guard rejects reentrant entry into the engine, so the phase contract is enforced at the boundary rather than by a per-read lock or version check. Reentrant or concurrently delivered changes (including parser, resource, animation, and environment changes that become ready mid-evaluation) are queued for a later transaction.

The read interval ends only after computed outputs and observer consequences commit atomically. Style node identities, program versions, and environment handles named by an active evaluation cannot be destroyed or reused before retirement; retired identities are released for reuse only at a later safe boundary.

## 5. Style node identity and tree representation

### 5.1 Identity

`StyleNodeID` is a document-local `u32`. A 32-bit identity is sufficient for one document and halves the size of many indexes and handles versus native pointers.

Pseudo-element matching projects declarations from an originating element's match set. A `PseudoElementTarget` carries the pseudo kind without allocating another style node. A first-class pseudo-tree representation may introduce its own identity model when view-transition pseudo-trees need one.

### 5.2 Column inventory

Do not blindly duplicate fields already on DOM elements. Persistent style-side storage is:

* a minimal style-data handle reachable from an element;
* required evaluator-local relation-navigation state;
* sparse segmented columns for optional per-element information;
* reusable temporary columnar batches for bounded-region evaluation;
* indexes containing only style-relevant facts.

```text
parent                       required, dense
first element child          required, dense
next element sibling         required, dense
previous element sibling     required, dense
tree scope                   conditional, allocated only for multi-scope documents
assigned slot                allocated per segment
shadow host                  allocated per segment
local feature-set identity   optional, Tier 3
style-record identity        Tier 1 for observed content
```

Optional columns are allocated per segment only when useful. A document with no shadow trees allocates no shadow relationship columns.

### 5.3 Required relation navigation

Transpose programs must traverse inverse selector relations without leaving the evaluator. That is why parent / first-element-child / next-element-sibling / previous-element-sibling are dense required columns rather than optional acceleration.

Concretely: for `.theme .item` and a class mutation on a container, the transpose step for the changed `A` operand is "descendants that can satisfy `B`". That set is not in the `TreeDelta`, so the evaluator must either stream the subtree from the authoritative side or intersect a resident `.item` posting with subtree membership. `StyleNodeID` is not a tree-order label, so membership costs `Hregion` relation steps per candidate. Without resident columns, every one of those steps is a boundary crossing, and the hot path degenerates into reverse cold requests or per-element FFI. Both are forbidden.

Rules:

* Dense `u32`, allocated for every connected style node, charged to **Tier 1**, counted against the mandatory per-node byte budget (§10.5).
* **Not semantically authoritative.** The exact cold evaluator can always read the live DOM. The columns are a derived projection that must agree with the live tree at every epoch boundary.
* Not evicted under ordinary memory pressure; Tier 3 is evicted first. If they cannot be allocated, evaluation stays exact through bounded live-fact batches.
* Ownership follows the tree: the subsystem that mutates DOM structure publishes relation-column updates as part of the same `TreeDelta` that reports the mutation, so they cannot drift within a read epoch. Mismatch is an invariant violation, not a recoverable condition.

Tree-scope navigation has the same traversal requirement once multiple scopes are supported, because scope-membership transposition works the same way. Allocate the dense tree-scope column only for documents that need it, and charge it to the conditional byte allowance (§10.5). Flat-tree operations derive from the sparse assigned-slot and shadow-host relations instead of duplicating a third parent column.

### 5.4 Temporary packing

Packing is an execution choice, not snapshot construction. Small or selective work queries the live tree directly. A large batch may gather only its requested columns and affected region into Tier-4 scratch when dense traversal, SIMD execution, or bridge amortization wins. Batches are generation checked, released after the transaction, and never authoritative. Whole-document cold evaluation streams bounded batches; it never allocates a complete flattened DOM copy.

### 5.5 Multiple tree relations

CSS does not operate over one universal tree. The logical input distinguishes:

* DOM ancestry
* shadow-including ancestry
* flat-tree inheritance relationships
* slot assignment
* part exposure
* pseudo-element origin relationships
* view-transition capture and generated pseudo-tree relationships
* style-scope membership

**Every selector operator states which relation it consumes.** This keeps special cases out of the generic parent and descendant operators. A generic descendant walk does not pierce a shadow root or follow slot assignment.

### 5.6 Document order

Build preorder ranges in transaction scratch for the scope being evaluated; retain no per-node tree-order label. Queries depend on relative order and ancestry, never on the numeric representation of an order label, so physical relabeling is invisible to semantic dependencies.

A subtree move emits relationship deltas for its boundary only. Internal relationships remain shared unless the move changes the relevant tree relation, scope, slotting, or inherited context.

## 6. Selectors

### 6.1 Element and document state

State is represented as semantic facts, not bespoke invalidation entry points:

```text
Hovered(element)      Active(element)        Focused(element)
FocusVisible(element) Checked(element)       Disabled(element)
Valid(element)        Target(element)        PlaceholderShown(element)
Open(element)         PopoverOpen(element)
```

A transition publishes old and new fact values. Only selector operators depending on that state receive the delta. Derived states such as `:focus-within` are logical operators over tree relationships; the same machinery supports `:has(:focus)`, `:has(:checked)`, and state nested in selector-list pseudo-classes.

High-frequency states use the same semantics but may be scheduled in a low-latency lane: a hover change must not wait behind compilation of an unrelated large stylesheet when its dependencies can be evaluated independently.

**Visited-link privacy.** Visited state is a routed state fact like any other; a dedicated privacy-taint component in operator, cache, and record identities is **not yet implemented**. When visited-dependent styling is implemented to specification, tainted materializations must not expose visited information through timing diagnostics, developer APIs, or cache keys observable across privacy boundaries, and untainted and tainted results must never share a proof merely because their value bytes match.

### 6.2 Selector logical IR

Selectors compile into a compact logical IR. The IR defines semantic equality independently of how an operator is evaluated. Representative operators:

```text
LocalFeature   And           Or            Not
Parent         Ancestor      NextSibling   FollowingSibling
NthPosition    RelativeExists
ScopeStart     ScopeEnd      Host          Slotted        Part
PseudoElementProjection      StatePredicate
```

### 6.3 Bidirectional compilation and delta routing

Every selector compiles into two views of the same semantics:

```text
match program:     candidate subject -> exact match result
transpose program: changed input     -> possible affected subjects
```

The match program is exact. A transpose program may over-approximate but must **never** omit a subject whose selector truth can change. It starts at each occurrence of the changed input in the selector and traverses inverse combinator relations toward the subject. Resulting subjects are checked by the match program before any selector-truth delta is emitted.

| Match fragment | Changed operand | Transpose step toward possible subjects |
| --- | --- | --- |
| `A B` | `A` | Descendants that can satisfy `B` |
| `A B` | `B` | The changed `B` node |
| `A > B` | `A` | Element children that can satisfy `B` |
| `A > B` | `B` | The changed `B` node |
| `A + B` | `A` | The next element sibling if it can satisfy `B` |
| `A + B` | `B` | The changed `B` node |
| `A ~ B` | `A` | Following element siblings that can satisfy `B` |
| `A ~ B` | `B` | The changed `B` node |

Compound operands transpose to the node whose local fact changed. Structural pseudo-classes add typed child-sequence entry points. `RelativeExists` transposes a possible witness change through the inverse relative-selector axes to possible anchors, then transposes an anchor-truth change through the outer selector to its subjects (§7). Every step names its DOM, shadow-including, flat-tree, slot, part, pseudo, or scope relation explicitly.

*Database counterpart:* the transpose program is the standing query's delta rule, its derivative with respect to one input: given that input's change, it enumerates the output rows that can change. Match-checking every enumerated subject is delta verification, and it is what makes safe over-approximation legal.

**Delta-routing registry.** Required program state mapping each semantic input key to the transpose entry points that mention it:

```text
(input kind, StyleAtomID or relation kind) -> transpose entry-point slice (stable order)
```

It covers tag, ID, class, attribute, namespace, state, tree relation, structural, scope/topology, and activation inputs. Selectors with no selective local atom go into a **typed** always-consulted slice for the input kinds that can affect them, not one global universal bucket. A child-list mutation must not consult programs that depend only on an environment predicate.

Construction and bytes are charged to program compilation and Tier 2. Sorted delta-coded program and entry-point IDs keep the structure proportional to selector-input incidence, not element count.

The registry is required, not optional: scanning every selector header for every ordinary mutation changes the hot-path asymptotics. Feature postings (Tier 3) may accelerate subject enumeration, but evicting them never changes which transpose entry points run. Program replacement keeps old registry entries alive through old-result removal and installs the new program and registry atomically in the next epoch.

### 6.4 Impact regions

For a normalized transaction `T` and attached program version `P`:

```text
Impact(T, P) = normalize(union(transpose_entry(delta) for delta in T))
```

The result can contain individual style nodes, sibling intervals, subtrees, tree scopes, and dependent flat-tree or shadow scopes. It is a transaction-local execution plan, never a retained element-by-selector relation. Overlapping regions coalesce without changing their named tree relation.

Every selector operator and every non-selector dependency that can request exact scope evaluation has a transpose rule satisfying:

> Between the last committed state and the final live state in the read epoch, every style node whose semantic selector, cascade, computed value, or observer output can differ is contained in `Impact(T, P)` or in an explicitly chained downstream region produced from it.

Coverage is fail-closed: any gap widens the region; unknown scope is never empty scope. Explicit epoch-bound impact certificates are not retained today; the coverage obligation is the same, enforced by widening and verification.

Consequences to implement:

* Inheritance transposes through the **flat** tree.
* Slot reassignment emits both participating light-tree and shadow-tree regions.
* A scope or stylesheet-topology change names every scope whose attached program or cascade order can differ.
* When an operator cannot construct a narrower proven region, it widens to that operator's complete tree scope, and ultimately to the document.

Regions carry attribution: a region attributed to a specific rule and selector program is eligible for exact per-entry planning, while an unattributed region is evaluated conservatively. Any gap (a missing posting, an incomplete retained answer, an unroutable input) surfaces as a typed `Missing`/`Incomplete` lookup whose only legal response is widening, ultimately to the document. A verification gate can additionally assert that a scoped plan publishes nothing it cannot attribute.

The exact batch plan consumes only a complete impact region.

### 6.5 Execution kernels

One selector semantics, three cooperating execution kernels. The boundaries are physical, so operators can move between them later without changing query meaning.

**Local-compound kernel**: the dominant hot path. Evaluates tag names, IDs, classes, attributes, namespaces, element states, and Boolean compound structure. Uses fixed-width interned atom IDs at the C++/Rust boundary, a rightmost-feature dispatch key, and compact fused bytecode. Candidate tests are ordered cheap/high-rejection first and short-circuit on failure. **A compound with no relational operator allocates no witness.** (Non-relational chains do participate in the retained prefix automaton below when admitted; that state is Tier-3 and evictable, never required.)

Selector-used features have two sparse directions with independently bounded sizes:

```text
feature atom -> candidate StyleNodeID posting     Tier 3, optional
feature atom -> transpose entry points            Tier 2, required, program-proportional
```

The first avoids scanning DOM facts and may be evicted. The second lets a class, attribute, tag, ID, or state mutation activate only bytecode mentioning the changed feature. If the posting is evicted, the routed transpose program reads authoritative DOM facts or uses an exact scope batch. No element-by-selector relation is ever required.

**Directional-combinator kernel**: child, descendant, adjacent-sibling, and subsequent-sibling relationships consume the same local compound programs, under operation-specific schedules:

* *Cold initial style*: stream style nodes, gather candidate program offsets from each node's local feature atoms, match relationships right-to-left with direct tree walks.
* *Dynamic selector insertion*: reverse direction: start from the new program's safest rightmost-feature posting and check only those candidates when selective.
* *Mutation to an attached program*: start from changed local truths. Descendant and child effects run as a top-down prefix-state pass over the exact affected scope, warm-started from the retained prefix states of the previous transaction where those survived.
* *Sibling effects*: adjacent-sibling inspects the immediately participating siblings; subsequent-sibling and structural effects use the same automaton's child-sequence machinery and stop at unchanged outputs.

These are schedules over the same bytecode.

**The prefix automaton.** Selector-prefix truths are retained across transactions as a Tier-3 cache: interned prefix states (the "context tokens" entering each node), per-element entering states, per-element terminal match sets, and per-element positional truth bits. Registration is budgeted: an automaton carries at most 32 structural-test truth bits, and a chain that would overflow is refused whole and stays with its exact routes, so refusal degrades to exact matching, never to a wrong answer. The cache's bytes cycle through scratch during a flush and are retained at the end of a patched flush; it is prioritized above the retained answers it maintains (§10.3), and discarding it is always legal; the next flush rebuilds from exact evaluation.

**Relational-query kernel**: `RelativeExists` reuses local compounds and directional traversal to answer an existential question per anchor. Witnesses belong only to this kernel. A document with no relational selector allocates no relational queues, per-node relational fields, or relational cache tables (the witness table is empty and unallocated, and relational routing early-returns on an empty route set).

Selector lists and Boolean functional pseudo-classes compile to compact branch bytecode around these kernels. Specificity and match metadata stay static program data rather than being reconstructed per candidate.

### 6.6 Local feature indexes

Index keys are semantic feature atoms, not strings: a tag/namespace pair, ID, class, attribute presence, attribute exact value with case mode, or state. Substring and token attribute operators drive from an attribute-name posting and run their exact value test in compound bytecode rather than demanding a posting per substring.

Representation: **chunked sorted postings**, sorted by `StyleNodeID`. The index contains only features used by an attached selector program; removing the last consumer makes it reclaimable.

*Database counterpart:* secondary inverted indexes, and non-clustered ones: `StyleNodeID` is not a tree-order label, so proving a posting candidate lies in a region is the row lookup a non-clustered index pays when the predicate lives on the clustering dimension.

Because `StyleNodeID` is not a tree-order label, a subtree impact region is *not* a numeric posting interval. For a selective plan, enumerate posting candidates and prove region membership through the named tree relation, charging ancestor/sibling steps explicitly. Those steps walk the required relation columns, so a selective plan costs `Hregion` in-evaluator pointer hops rather than boundary crossings. For a broad region, stream the region in preorder and test local features from authoritative facts instead of building a full posting-to-preorder join.

Transaction-local preorder arrays make repeated relation checks constant time only after their construction cost and `StyleNodeID`-to-preorder mapping bytes are charged to discovery and Tier 4.

### 6.7 Selective-plan cutoff

The plan choice is made once, before enumeration, from candidate cardinality and region size: **the exact batch plan is chosen when the smallest safe candidate source covers more than one quarter of the region** (`SELECTIVE_SHARE_DIVISOR = 4`). A candidate source of 64 or fewer skips the cost model entirely and goes straight to direct membership checks.

The cutoff uses **exact cardinality** of normalized impact ranges. If exact cardinality is unavailable, the batch plan is chosen rather than a smaller scope guessed. Scratch or Tier-3 refusal can independently force the batch plan (or, when no batch is executable, the conservative region) before enumeration begins.

### 6.8 Query outputs

```text
SelectorMatches(SelectorProgramID, StyleNodeID, MatchMetadata)
```

`MatchMetadata` contains only what later semantics need (selector-list entry, specificity, scope proximity, pseudo-element target) and is factorized when fields are static for the selector.

Rule matching is a join:

```text
SelectorMatches
AND RuleUsesSelector
AND RuleAttachedToScope
AND ActivationPredicateIsTrue
    -> ActiveRuleMatches
```

This separation lets declaration edits, condition changes, and attachment changes reuse selector truth.

Concrete matches are enumerated into a transaction-local sorted `StyleNodeID` vector and discarded after cascade consumption.

### 6.9 Specificity

Specificity is program metadata with explicit aggregation rules. Each top-level complex selector in a rule's selector list carries its own static specificity. If multiple entries match the same style node, `MatchMetadata` retains the greatest matching entry specificity together with any independently required scope and pseudo metadata. Short-circuiting may skip work only when it cannot hide a later matching entry with a greater cascade contribution.

`:is()` and `:not()` contribute the maximum specificity of their argument list as a static property of the program, regardless of which branch establishes the Boolean result. `:where()` contributes zero. `:has()` uses the corresponding static argument-list rule even though witness discovery is dynamic. Structural and nesting operators use their specification-specific contribution; none of them inherit a generic runtime "specificity of whichever branch matched" rule.

The cold evaluator and the incremental evaluator consume the same static specificity table.

### 6.10 Ephemeral selector consumers

Selectors passed to `querySelector()`, `querySelectorAll()`, `matches()`, and `closest()` are **match-only query programs**. They compile with the same selector semantics and evaluate with a bare match evaluator over the engine's fact store, but they are not attached style programs: no transpose bytecode, no routing registry entries, no rule identities, no witness reads or writes, no cascade state. Scope roots, shadow boundaries, relative-selector anchoring, and syntax failure are explicit compiler inputs. Result ownership belongs to the DOM API, not StyleEngine.

Two mechanisms make this correct and fast against a live document:

**Settling.** A connected query must see the current DOM generation, which may have pending staged input. Preparing a query commits the pending fact rows into the resident store while stashing their pre-images; the next flush (or normalize) restores the pre-images first, so the style transaction still computes its before/after diff as if the query had never intervened. A query observes current facts; invalidation keeps its native orientation.

**Isolated engines for disconnected roots.** A query rooted in a disconnected tree runs against a small throwaway engine populated from that subtree alone. These engines are cached per root (an LRU keyed by root and mutation versions) so repeated queries over the same detached tree reuse one population, and a compiled query program is cached for the lifetime of its `SelectorQuery` object rather than recompiled per call.

### 6.11 Push/pull division

Push exact deltas only when the transaction or a resident view (witness, winner column) directly names the consumer. Everything else is validated by exact pull evaluation of the affected scope.

When explicit subscriber fanout would cross a hard edge threshold, do not allocate the edge set; pull from the exact affected scope instead.

## 7. Relational selectors and `:has()`

`RelativeExists` is an existential query relating an anchor to a relative-selector witness. Its semantic output is Boolean even when many witnesses exist. **Only transitions between zero and nonzero witnesses can affect selector truth.**

### 7.1 Compilation

A relative selector compiles into operators over: anchor scope; child or descendant traversal; adjacent or following sibling traversal; ordered sibling-descendant combinations; local compounds and selector-list logic. Every traversal operator names its tree relation and scope boundary.

### 7.2 Simple-query grammar

A `:has()` query is **simple** when all of:

* the relative selector contains exactly one axis (a selector-list argument compiles each branch into its own relative query, each classified independently);
* that axis is one of: implicit descendant, child, adjacent sibling, or following sibling;
* that axis is followed by exactly one local compound;
* the compound contains at least one positive indexable driving feature;
* the query contains no structural pseudo-class, nested relational operation, internal combinator, pseudo-element projection, or shadow/scope-crossing construct.

Simple: `:has(.error)`, `:has(> .item:hover)`, `:has(+ .selected)`, `:has(~ .selected)`. Complex: `:has(.group > .item.error)`, `:has(:nth-child(2))`. A list such as `:has(.a, .b)` is two simple queries, each with its own axis and driving feature.

Complex ≠ unsupported. Complex queries use the exact direct relative-selector evaluator and retain no witness.

Selectors Level 4 forbids nesting `:has()` inside `:has()`; reject that syntax at compile time rather than treating it as an incremental-coverage case.

### 7.3 Witness state

Retain **at most one witness per simple positive observed query**. Consequences:

* Adding another witness does not change state and emits nothing.
* Removing a non-retained witness requires no repair.
* Removing the retained witness triggers a replacement search.

A retained witness is a proof cache, never semantic state; evicting it is always legal.

*Database counterpart:* `:has()` truth is an EXISTS aggregate, so only zero-to-nonzero transitions are deltas. The retained witness is a one-row partial materialization of the subquery; losing it is a view miss repaired by an upquery, never a semantic event.

**Bounds.** A retained witness is re-verified on read (liveness, axis membership, and a match re-check), with a fixed step cap on the following-sibling back-walk; any verification failure clears the entry and routes conservatively. The witness table itself is capped (currently 16,384 entries) and is dropped whole on a refused memory grant. Replacement after a lost witness is the ordinary forward walk of the query's axis; the routing-side cutoff (§6.7) bounds how witness-side discovery drives anchors. A Boolean result is never approximated.

### 7.4 Negative results

A negative `:has()` result must not allocate dependency edges to every element in the searched region, and it allocates nothing at all: a query that finds no witness stores no state. Absence is recomputed on demand, and the false-to-true transition is caught from the witness side: a mutation that could create a witness routes through the relative-selector transpose entry points to its possible anchors (§7.5), so there is no cached negative to invalidate and no stale negative can exist.

### 7.5 Witness creation (false-to-true)

When a local mutation can create a witness, routing activates the relative-selector transpose entry point. For a simple query, possible-anchor discovery is bounded by the single inverse axis:

| Relative axis from anchor | Possible anchors for a newly matching witness |
| --- | --- |
| Child | Its element parent |
| Descendant | Its element ancestors up to the query's scope boundary |
| Adjacent sibling | Its immediately preceding element sibling |
| Following sibling | Its preceding element siblings in the same child sequence |

Each possible anchor is checked exactly, including anchor compound and scope semantics. A false-to-true Boolean transition then enters the outer selector's transpose program, so `.card:has(.error) .button` reaches only possible `.button` subjects in the affected anchor context. A new additional witness for an already-true anchor emits no Boolean delta and never runs outer propagation.

Complex relative selectors use the same principle with multiple inverse axes; their transpose program may widen to a sibling interval, ancestor path, subtree, or complete tree scope, but never omits a possible anchor. Discovery work is charged even when every anchor check rejects.

### 7.6 Non-subject `:has()`

For `.card:has(.error) .button`, the relative query produces a Boolean anchor result that updates the anchor's outgoing top-down selector context. Descendant `.button` matches change only where the context transition changes their rule output. Adding a second `.error` while a witness already exists emits no anchor-truth delta and therefore no descendant selector work.

## 8. Stylesheet program

### 8.1 CSSOM versus semantic program

* CSSOM objects preserve API identity, ownership, indices, and serialization behavior.
* The semantic program is an immutable, content-keyable representation used for computation.

Editing a CSSOM object creates a new semantic version without replacing the wrapper object.

### 8.2 Stable identities

```text
SheetID              stable semantic sheet identity
RuleID               stable semantic rule identity
SelectorProgramID    immutable compiled-selector identity (content-interned:
                     equal compiled selectors share one ID)
DeclarationBlockID   declaration-block version (a fresh identity per edit)
CascadeLayerID       layer identity
ProgramVersion       one document-global program version, bumped per change
```

CSSOM wrapper objects hold their semantic identities on the C++ side (a stylesheet knows its `SheetID`, a rule its `RuleID`). A declaration edit preserves `RuleID` and `SelectorProgramID` while installing a new `DeclarationBlockID`; a selector edit preserves the rule identity but changes the compiled selector program and match relation. Rule contents are updated in place under the rule's single version slot; there is no retained history of prior rule versions, and nothing reads one: evaluation always runs against the current program, with old truth supplied by the journal's before-images and retained derived state (§4.2). Activation state (media, supports, container, sheet disabled) is tracked as current condition booleans per rule and sheet, updated by typed inputs.

### 8.3 Rule version contents

```text
RuleID
SelectorProgramID
DeclarationBlockID
conditions-hold flag
CascadeOrigin
CascadeLayerID
StyleScopeID
StyleSheetOrderToken
NestedRuleOrderToken
```

Selector list entries may carry separate static specificity and matching output identities while sharing a declaration block. Nested group rules form a persistent tree: editing one descendant creates new nodes only along the path to it; unchanged selectors, declarations, and conditions keep their identities.

### 8.4 Attachment

Attachment connects a compiled program to one or more style scopes and is distinct from parsing and compilation. It covers parser-created `style`/`link` elements, dynamically inserted stylesheet nodes, load completion, constructed stylesheets, `adoptedStyleSheets` changes, sheets shared by multiple shadow roots, enabling/disabling, and UA/user/author origins.

One compiled program attaches to multiple scopes without duplicating its logical selector or declaration representation. Scope-specific indexes and materializations stay separate where tree membership requires it.

Match and transpose bytecode contains document-local `StyleAtomID` values and is therefore **compiled per document**. Routing registries, attachments, order tokens, and all result materializations are always document-local. Authoritative parsed stylesheet resources may be shared across documents, but nothing carrying one document's IDs, generations, scopes, or privacy state crosses a document boundary.

### 8.5 Source order

Source order composes from per-axis components:

```text
origin              a fixed enum ladder (with importance reversal)
tree context        encapsulation-context depth
layer order         per-scope layer ranks, rebuilt from the ordered layer list
stylesheet order    order-maintenance tokens, one axis per tree scope
nested rule order   order-maintenance tokens, one axis per sheet
```

The two order-maintenance axes use `u64` gap labels: insertion writes one midpoint label, and exhausting a gap relabels the axis. Numeric relabeling changes no semantic version, because comparisons depend on relative order only.

Moving a sheet updates its sheet-order token; rules refer to that token indirectly, so their program nodes are not rewritten. Moving a nested rule updates the smallest enclosing order token.

Reordering affects selector truth only if scoping or attachment changes. Otherwise it is purely a cascade priority delta.

*Database counterpart:* gap-labeled ordering keys (fractional indexing). Comparisons read relative order only, so relabeling is a physical operation with no semantic version change.

### 8.6 CSSOM mutation surface

Every operation maps to a typed program delta. None produces generic document invalidation.

| Mutation | Primary semantic delta |
| --- | --- |
| Attach, detach, or move a `style`/`link` element | Attach, detach, or reorder a sheet program |
| Change text inside a `style` element | Structurally replace the sheet program |
| Finish loading a linked or imported sheet | Attach the compiled program at its semantic position |
| Change `href` on a stylesheet link | Detach the old resource; later attach the new program |
| Change `media` or `disabled` | Update an activation predicate |
| Reorder `adoptedStyleSheets` | Update sheet-order tokens |
| Add or remove an adopted sheet | Attach or detach an existing compiled program |
| `insertRule()` (incl. grouping rules) | Add a rule program at a new order token |
| `deleteRule()` (incl. grouping rules) | Remove a rule program |
| Change `selectorText` | Replace the selector-program edge of one rule |
| `setProperty()`, `removeProperty()`, declaration `cssText` | Replace one declaration block |
| Change a condition rule's text | Replace an activation predicate |
| `replace()` / `replaceSync()` | Structurally replace a sheet program |
| Edit keyframes through CSSOM | Update the named animation program and its consumers |
| Register a custom property | Update the registration input for that property name |
| Change the `style` attribute | Replace one element declaration block |
| Change an HTML presentational-hint attribute | Replace the mapped element declaration block, plus any independent selector/attribute inputs |
| Change an SVG presentation attribute | Replace the mapped element declaration block, plus any independent selector/attribute inputs |

CSSOM collections and wrapper objects reflect API-required mutations synchronously; semantic style work may remain queued until the next required observation. Stable object identities plus immutable program versions let those coexist.

### 8.7 Insertion

1. Parse or receive the immutable stylesheet program.
2. Compile selector, declaration, condition, layer, and scope nodes.
3. Assign a stylesheet order token between neighbors.
4. Attach the program to the applicable style scope.
5. Evaluate only selector-program nodes whose results are not already available in that scope.
6. Gate selector matches through the sheet's activation predicates.
7. Add exact matched-rule deltas to the cascade.

No existing selector truth is invalidated because a sheet was inserted. If the new declarations lose the cascade everywhere, computed styles are unchanged even though the sheet matched many elements.

**Batch planning.** A program transaction is planned as a batch, not a loop over inserted rules. Compile all new selector fragments, then choose selective or exact-batch evaluation once:

* A small number of selective rules drives evaluation from sparse feature postings.
* A large sheet or burst of sheets builds temporary rightmost-feature buckets and runs one columnar pass over the relevant live style nodes, evaluating all new fragments together.

Temporary buckets and candidate arrays are Tier-4 scratch, discarded afterward.

If no style was observed before parser or script mutations completed, compute only the final program and style state. If an observer required an intermediate state, apply later sheets incrementally to that committed state.

**Loading and `@import`.** Network completion order does not determine cascade order. A linked or imported sheet receives its semantic position from document and rule order before or while it loads; when contents arrive, attachment uses that existing token. An `@import` rule is an import program node carrying layer, supports, media, and source-order inputs; load completion attaches the imported program beneath it without rebuilding or renumbering the parent sheet. Several sheets completing before the next observation join one normalized transaction while keeping their independently established positions. If script forces style between completions, each observed completion commits as its own transaction.

Parsing and immutable program compilation may run ahead of attachment, off the main style-evaluation path, subject to CSSOM visibility and loading-order requirements.

### 8.8 Removal

Removing a sheet detaches its rules from the scope. Determine affected matched rules **without** retaining a complete inverse match relation per rule:

* Reject a removed rule immediately when the complete winner-group column proves it won no declaration.
* Otherwise route the detached selector programs from their indexed subject features and evaluate them exactly before retiring them.

Bulk removal is grouped: detaching a sheet or rule subtree evaluates its program-owned selector set as one change and removes its declarations from cascade structures in one operation. Cold content validates against the new program version lazily. Removing a sheet with no winning declarations emits no computed-property deltas.

### 8.9 Rule-level edits

**`insertRule()`** adds one immutable rule version at an order token between neighbors and compiles and evaluates only the new selector and condition fragments. Existing rules are neither renumbered nor rematched.

**`deleteRule()`** removes the rule from its matching elements and repairs cascade winners for the properties it declared. A cold deleted rule is re-evaluated to enumerate its affected elements. Deleting a group rule detaches the persistent subtree rooted at that group; shared descendants stay alive if referenced elsewhere.

**`selectorText`** replacement keeps the `RuleID` and order token, compiles the new selector program, and records one typed input carrying the old and new program identities. Both programs are then evaluated through the retained-answer patch machinery: old-program matches produce removals, new-program matches produce additions, and the cascade-winner comparison stops propagation where the outcome is unchanged, except that an element whose recorded winner came from the edited rule always takes the full cascade reduction, since the fast comparison cannot prove its winner survived. Answer identities are keyed by program, so an element matching both the old and new selector still transits two answer identities; the stop happens at the winner layer, not the match layer.

**Declaration change** does not invalidate selector truth. A resident exact match relation feeds the declaration-block delta directly into the cascade; if absent, exactly reconstruct only the edited rule's matches and charge that selector work to `reconstruct`. Never rematch unrelated rules. Only properties present in the old or new declaration block are winner-change candidates.

Inline style uses the same declaration-block mechanism, with a match relation containing exactly its originating style node. HTML presentational hints and SVG presentation attributes use the same element-owned relation with distinct declaration kinds and their spec-defined cascade placement. They do not masquerade as inline style and do not enter the selector program merely because their source syntax is an attribute.

### 8.10 `replace()` / `replaceSync()`

Replacement reuses rule identities **positionally**: the sheet's style rules are snapshotted, and each incoming style rule reclaims the next prior `RuleID` in order, carrying its old declaration block forward and recompiling its selector. Compiled selector programs are content-interned, so an unchanged selector keeps its `SelectorProgramID` and its match relation for free. A sheet containing any non-style rule (or a rule-kind mismatch mid-list) falls back to whole-sheet detach-and-reattach, which really does swap the rule list; that path still compares selector truths and winners before emitting downstream changes, so correctness never depends on the reuse heuristic finding a correspondence. A content-based structural diff that survives insertions at the top of the sheet is a future idea.

### 8.11 Layers and conditional rules

Cascade layers form their own versioned topology. Layer statements, nested layers, and sheet attachment can change relative layer priority; rules reference layer identities rather than copied numeric ranks. A layer topology change updates property-specific cascade comparisons for participating matched declarations and does **not** invalidate selector truth. Important declarations use the reversed layer ordering through the cascade-priority operator.

Program deltas are typed for: `@media` and sheet media attributes, `@supports`, `@container`, `@scope`, `@starting-style`, `@property`, `@font-face`, `@keyframes`, `@counter-style`, `@font-feature-values`, namespace and layer statements. Each publishes deltas to its semantic consumers: changing a keyframes rule begins at animations referencing its name, not at every element in the scope.

## 9. Cascade and computed values

### 9.1 Match-answer identities

Comparable selector answers are immutable and interned under a compact integer identity. The identity names the normalized `RuleMatch` vector independently of the style node it was evaluated for, so equivalent answers can reuse cascade expansion and prefix-cache results without copying or renormalizing the vector.

These identities are document-local acceleration, not a retained per-element match column. A style ask publishes only the identity of the answer it just produced, and answer caches may carry that identity to their consumers. Program changes reach their candidates through selector dispatch and exact evaluation of the old or new program as appropriate. Do not retain a complete `RuleID -> elements` relation; inverse match sets are optional query-level materializations only.

### 9.2 Cascade priority

A declaration's priority is a comparison program over stable identities:

```text
origin and importance
encapsulation context
element-declaration kind and language-defined placement
layer topology
specificity
scope proximity
source order
```

The element-declaration component distinguishes inline style from HTML and SVG presentational hints; the mapping layer supplies correct origin, specificity behavior, and position relative to stylesheet rules. Components that change globally (layer or stylesheet order) are referenced indirectly, so updating a topology token does not rewrite every declaration.

### 9.3 Provenance and candidate access

For each observed style node, retained cascade state identifies each property's winning source at rule granularity:

```text
color       <- rule 955
display     <- rule 17
background  <- element declaration (inline style)
--theme     <- exact cascade (reconstructed cold)
```

Representation: an **interned winner group** per observed style node (Tier 3, may be absent). Sparse and shared, never one heap object per property per element. Only properties declared by an added, removed, modified, or reprioritized rule are considered.

Candidate access is exact in each mutation direction:

* **Rule insertion** gets concrete matches from evaluation of the inserted selector; each matched node compares only that rule's declarations against its existing winners.
* **Rule deletion** stops immediately when complete winner state proves the rule won nothing; otherwise it evaluates the old selector before retiring its program. A deleted *winner* is repaired by exact cold cascade reconstruction.
* **Declaration editing** preserves the rule's selector relation; reconstruct it exactly if not resident, then apply the declaration delta.
* **Reprioritization** uses complete winner state to reject unaffected rules, then runs an exact affected-scope batch pass. A real broad order change is allowed to cost broad work.

*Database counterpart:* a winner is a top-1 aggregate per (style node, property) under the priority order, and top-1 is the classic non-invertible aggregate: insertions and losing deletions repair in place by comparison, but deleting the current winner cannot be repaired from the aggregate alone. That is exactly why a deleted winner falls back to exact cold cascade reconstruction, the same rescan MIN/MAX maintenance performs under deletion.

The exact cold cascade gathers all active declarations for one node and property and runs the same priority comparison program as the incremental path, depending on no winner caches, tournament nodes, or retained state. It is both the eviction path and the reference implementation.

### 9.4 Winner stopping keys

Propagation stops at the earliest layer that can prove the output unchanged. The stack, from cheapest to most expensive:

1. **Answer-identity stop.** A patched retained answer whose identity is unchanged emits nothing; a cohort memo lets one proven transition answer for every node sharing the same delta shape.
2. **Cascade-input stop.** The compacted match answer (non-winning author rules dropped) is compared; equal inputs cannot change winners.
3. **Winner-key stop.** Per property-group winners carry a `SpecifiedWinnerKey`:

```text
SpecifiedWinnerKey
|-- canonical specified-value identity
|-- CSS-wide cascade operator and its continuation point
`-- animation and transition relevance
```

Declaration identity and source position are excluded, which is what makes theme swaps that produce identical values free downstream. Transition and animation relevance *are* semantic and participate. The winner comparison produces a semantic delta; an empty delta with a valid previous computation stops before value resolution.
4. **Reuse gating.** The winner-key stop may reuse the previous computed result only when the previous computation's recorded inputs still hold: its input record pins the values and parent groups it read, records which inherited custom-property bindings the computation actually consulted, and carries a conservative `read_beyond_the_record` bit covering every dynamic dependency not individually tracked (container, attributes, sibling position). The bit defaults to "incomplete", so an untracked dependency forces resolution rather than a wrong reuse. Live transitions and changed custom-property environments likewise force resolution.
5. **Dependency masks after resolution.** When winners did change, the semantic delta narrows downstream work to the affected computed groups and property words rather than rebuilding the whole style; final record-identity equality then lets unchanged payloads be shared.

### 9.5 Cascade-wide keywords

`inherit`, `initial`, `unset`, `revert`, and `revert-layer` are cascade **operators**, not eagerly flattened values, so their dependencies on parent style, origin, and layer topology stay explicit.

A winner query is parameterized by its cascade ceiling and exclusions: `revert` resumes below the current origin; `revert-layer` resumes below the current layer in the applicable origin and importance ordering (important declarations use reversed layer order). A topology edit changes the referenced layer-order identity. Winners carrying a revert continuation are excluded from the retained fast paths outright and always evaluate cold, which subsumes repair.

Do not implement these as one-time value substitution; that hides the dependencies needed for incremental repair.

### 9.6 Computed-value dependency graph

The stage consumes changed cascade winners plus non-cascade semantic inputs and evaluates only the affected dependency closure. Static dependency metadata is generated with the property implementation; dynamic edges are discovered while evaluating values.

```text
font-size           -> em-based values
root font-size      -> rem-based values
writing-mode/direction -> logical property mapping
color               -> currentColor consumers
custom property --x -> var(--x) consumers
attribute x         -> attr(x) consumers
container metrics   -> container units and queries
viewport metrics    -> viewport units
font environment    -> font-relative metrics
anchor geometry     -> anchor functions
tree position       -> tree-counting functions
```

### 9.7 Custom properties

Custom-property environments live on the C++ side (`CustomPropertyData`); the engine interns only an opaque environment identity per record. Environments are immutable and structurally shared:

* Identity zero means the empty environment.
* An environment is its declared name/value map plus a parent pointer: inherited changes are a parent environment plus a small delta, and lookup walks the parent chain.
* Two flattening heuristics bound chain cost: a chain deeper than 32 flattens fully, and a small parent (8 declarations or fewer) is absorbed into its child.
* Environments are interned by parent identity plus declared names and values, so most descendants inherit the same identity and allocate nothing.

Resolution is memoized per environment: one cached resolution per (environment, document, registration generation), where the registration generation is document-wide and bumps on any `@property` change, so a registration edit invalidates environment resolutions wholesale rather than per name. Cycle handling follows the specification's property-replacement algorithm with an active-resolution guard producing the guaranteed-invalid result. The inherited custom-property names a computation actually consults are recorded in its input record and participate in the reuse gate (§9.4).

A separate **parsed-substitution cache** memoizes the parsed value of substitution-bearing declarations per (rule, environment identity, registration generation, property): elements sharing an environment parse each `var()`-bearing declaration once per restyle instead of once per element. It is invalidated by registration-generation change (wholesale), by declaration edits (per rule), and by environment death (swept). It is currently accounted outside the engine's memory controller, a known gap.

Environment lifetimes are reference-driven: a live style record or input record pins its environment (and the parent data it read), and a sweep reclaims environments with no remaining reader. Unbounded history retention is forbidden.

### 9.8 Computed-style records

The engine interns computed style as a 32-bit base record: the tuple of a computed-group set, custom-property environment, fixed metadata, and reconstruction metadata, each itself an interned handle. What an element carries is the 64-bit final record identity: the base record plus a tag bit and generation distinguishing animation overlays, so an animated element's identity changes with its overlay while sharing the base. Sharing comes from hash-consing at publication; the group payloads reuse the existing Rust computed-value group representation, and no second complete style layout exists.

Layout and paint consume the same style handle rather than retaining redundant complete style objects, where lifetime and threading permit.

### 9.9 Inheritance

Inherited values flow through a shared inherited-context identity. Use the inherited-group handle already present in the parent style record and perform a sequential top-down comparison over the required observed subtree. Descendants are **not** given copied inherited payloads or permanent dirty bits. A descendant observed under a new token evaluates only inherited properties it consumes and stops when its resulting inherited or full style identity is unchanged.

Inheritance transposes through the **flat** tree.

### 9.10 Activation and environment predicates

```text
SelectorMatches
AND SheetAttached
AND MediaConditionTrue
AND SupportsConditionTrue
AND ContainerConditionTrue
AND ScopeConditionTrue
    -> ActiveRuleMatches
```

An environment change can toggle already-known matches without re-running selector logic. Representation: compact predicate bytecode plus one current result per attached group (Tier 3, evictable, reevaluated from typed environment inputs). Do not build a persistent inactive-rule match cache; activation evaluates the selector program through normal inputs, reusing a cached selector answer only when one is already available in the current traversal.

Environment inputs: viewport and page size; device pixel ratio; media features and preferences; document URL and target state; font selection and loading state; container sizes and styles; scroll and view timeline state; anchor-positioning inputs. Changes publish typed deltas to actual consumers.

### 9.11 Animations and transitions

Animations and transitions are **sparse time-dependent overlays on a stable base style**. Sampling an animation must not clone or recascade the base computed style.

The animation graph depends on the selected keyframes program, timeline inputs, animation and transition declarations, and base computed values required for interpolation. Changing `@keyframes` updates animations referencing the affected name; unrelated rule changes do not rebuild animation programs. When an animation completes or ceases to affect a property, removing the overlay reveals the already-maintained base value.

`@starting-style` contributes a typed starting-style program used only when the transition model requires it. Its rule and CSSOM mutations update before-change-style and transition-start consumers; it never becomes an unconditional alternate cascade for every element. Define the exact before-change-style lifetime and observation rules alongside the transition implementation.

### 9.12 Style-layout fixed points

The pure style graph contains only dependencies resolvable from inputs held stable by the read epoch. Geometry and layout-dependent features (container queries, anchor functions, tree-counting functions with layout inputs) belong to a separate style-layout fixed-point coordinator and cross an explicit typed boundary that exposes style-to-layout and layout-to-style version changes.

Use the existing feature-specific sequential coordinator; do not build a generic SCC runtime. The general selector and cascade graph remains acyclic within one read epoch. A cycle (style → layout → query input) produces one immutable style result per read epoch; the coordinator advances the layout-input version and schedules the next transaction per the feature's specified iteration and cycle-breaking rules. No partially updated graph is visible to script.

## 10. Memory

Memory is a primary constraint. Time improvements requiring unbounded retained state do not satisfy this design.

### 10.1 Tiers

* **Tier 0: authoritative semantic inputs.** DOM, CSSOM, stylesheet programs, browser state. Reference it; do not duplicate it.
* **Tier 1: minimal live style state.** Compact handles and shared payloads required to answer current observers. Not evictable without first proving the content has no observer.
* **Tier 2: shared semantic IR and intern pools.** Canonical selector, declaration, condition, cascade nodes; routing registry; transpose bytecode. Bounded relative to the compact parsed stylesheet program; cannot absorb selector-result state.
* **Tier 3: acceleration materializations.** Indexes, match sets, inverse maps, witnesses, proofs, flattened environments. Strictly budgeted and fully evictable.
* **Tier 4: transaction scratch.** Reusable arenas for flattened relation ranges, delta queues, batch evaluation, comparison. High-water marks monitored; shrinks after unusually large work.

An interned object is not automatically Tier 2. Contexts, summaries, match answers, and proofs stay Tier 3 even when stored in an intern pool.

| Retained object | Tier | Reclamation rule |
| --- | --- | --- |
| DOM, CSSOM, browser state | 0 | Owned by the authoritative subsystem |
| Active read-epoch header and generation | 1 | Reclaim after every reader retires |
| Attached compact semantic rule program | 2 | Reclaim after detachment and epoch retirement |
| Style feature atom mapping | 2 | Reclaim after last program/posting reference and epoch retirement |
| Delta-routing registry and transpose bytecode | 2 | Reclaim with the selector program and retired epochs |
| Stable rule, attachment, and order nodes | 2 | Reclaim after semantic detachment and epoch retirement |
| Memory controller and aggregate accounting | (uncharged) | Reclaim with the document |
| `StyleNodeID` and DOM-to-style-node mapping | 1 | Reclaim after disconnection and epoch retirement |
| DOM relation-navigation columns | 1 | Reclaim with the style node; not evicted for ordinary pressure |
| `StyleRecordID` on an observed style node | 1 | Reclaim only after the node is provably unobserved |
| Live shared computed-style payload | 1 | Reclaim after all style records and epochs release it |
| Live custom-property environment | 1 | Reclaim after styles, animations, epochs release it |
| Live animation or transition overlay | 1 | Reclaim after the effect and observing epochs release it |
| Cascade winner group | 3 | Evict and reconstruct through exact cold cascade |
| Feature posting | 3 | Evict and read authoritative DOM facts |
| Retained match answers and selector incidences | 3 | Evict and reevaluate their selectors |
| Prefix transition and answer caches | 3 | Discard; rebuilt from exact evaluation |
| Retained witness | 3 | Evict without semantic effect |
| Normalization journal and coarsened scope markers | 4 | Release after the normalized transaction commits |
| Temporary preorder, impact region, batch bucket, delta queue | 4 | Release at transaction end or scratch shrink |

### 10.2 Document budget

Each document has a derived-style memory controller tracking exact bytes by logical operator, physical representation, scope, and tier.

**Tier-3 allocations reserve their full capacity charge before allocation or growth.** The controller evicts enough existing capacity first; if it cannot, the requesting view is not built and evaluation continues cold. Temporary over-budget allocation followed by eviction is forbidden.

```text
Tier3Limit = min(
    DeviceCap,
    BaseAllowance
        + NodeAllowance * ConnectedElementCount
        + StylesheetAllowance * CompactStyleProgramBytes)
```

`CompactStyleProgramBytes` is the byte length of a minimal non-commoned encoding of the attached selectors, match and transpose bytecode, routing registry, declarations, and conditions, counted once for an explicitly shared constructed program. It **excludes** allocator padding, optional indexes, results, and StyleEngine's own capacity, so acceleration overhead can never inflate its own allowance. `ConnectedElementCount` counts connected styleable DOM elements in the live DOM at the read epoch, not pseudo style nodes, arena capacity, or retired generations.

| Configuration | Device cap | Base | Per connected style node | Per program byte |
| --- | ---: | ---: | ---: | ---: |
| Browser document | 64 MiB | 1 MiB | 2,048 bytes | 2.00 bytes |

These are a ceiling for the complete set of optional views, not an expectation. Counters report actual capacity against the limit rather than treating the limit as a target. Ignoring the program term, the node coefficient reaches the device cap at 32,256 elements; above that the cap binds and larger documents run progressively colder, which is intended.

Tier 2 is required program state and is tracked without a cap. The cold-interpreted fallback that a cap would require does not exist; required program capacity must therefore never be refused or relabelled as Tier 3.

**Tier 4 peak cap:**

```text
Tier4Limit = min(
    ScratchCap,
    max(4 MiB, Tier3Limit, TransactionNodeAllowance * ConnectedElementCount))
```

The scratch cap is 32 MiB and the transaction node allowance is 768 bytes. The multiplier on `Tier3Limit` is 1, not 2, so transaction scratch cannot dominate the document's total style footprint; the 4 MiB floor matters most on small documents. Tier 4 is a **reported ceiling**, not a refusable cap: required scratch charges cannot be refused (the flush must complete), so the limit exists to make an over-limit transaction visible in the pressure report rather than to stop it. The one refusable Tier-4 allocation is the optional preorder topology, whose fallback is simply planning without it.

During an active capture (and its replay), the memory policy pins the Tier-3 limit to the device cap so recorded eviction decisions are reproducible; ordinary builds never run that policy.

Byte accounting happens at arena, slab, vector-capacity, bitmap, and hash-table allocation boundaries: operators update aggregate counters when capacity changes, not on every lookup.

### 10.3 Eviction

Eviction removes physical acceleration state and bumps no semantic versions. A later observer reconstructs from authoritative inputs and remaining logical state.

The policy is benefit-driven with scan resistance built in. Each Tier-3 category accumulates hit/miss observations (with exponential decay); an eviction happens only when a refused requester has itself demonstrated nonzero benefit, and the victim is chosen among categories with **zero** observed benefit, in a fixed preference order (retained witnesses, feature postings, specified values, winner groups, selector incidences, retained answers, prefix caches). If evicting every zero-benefit category still cannot make room, nothing is evicted and the requester stays refused: an all-or-nothing rule that keeps one broad cold pass from flushing a working set that pays.

One deliberate exception outranks benefit: the prefix-state cache may evict retained answers and winner groups to stay resident, because the maintenance structure outranks its derivatives.

The budget responds to document and program size. Tier-3 state remains fully evictable while compact live styles are retained.

*Database counterpart:* buffer-pool replacement, with its known trap: without scan resistance and cost awareness, one broad cold pass flushes the working set, and a recency-only policy starves the views whose maintenance would pay, leaving a full-scan engine with extra bookkeeping.

### 10.4 Interning

Intern pools use compact document-local handles and weak reclamation where possible. Apply hash-consing only where lookup and table overhead is likely to beat duplication; very small or unique values are cheaper inline. Prefer adaptive small-vector and interned representations over universal heap allocation.

### 10.5 Per-node metadata budget

Mandatory per-node state is at most **six 32-bit words**:

```text
StyleNodeID or its DOM mapping       (an existing DOM identity can satisfy this without duplication)
StyleRecordID                        observed content only
parent                               required relation column
first element child                  required relation column
next element sibling                 required relation column
previous element sibling             required relation column
```

```text
MandatoryNodeBytes(surface) = BaseMandatoryNodeBytes + ConditionalRelationBytes(surface)

BaseMandatoryNodeBytes           <= 24
ConditionalRelationBytes(surface) <= 8
MandatoryNodeBytes(surface)      <= 32
```

The engine asserts the relation-column budget in its own tests. The conditional allowance covers tree-scope identity, allocated only when the document requires it and no authoritative field can be exposed safely without duplication. Slot, part, pseudo, or future relation navigation must derive a compact identity or replace the physical representation; it cannot raise the 32-byte cap. (`StyleNodeID` is a `NonZeroU32`, so optional relation slots niche-pack into one word.)

Optional context, winner, dependency, and witness handles live in sparse Tier-3 columns and do not consume a reserved word on every node. Shared live style payloads are reported separately.

**Tier 1 is required state, not free memory.** No duplicate complete computed styles.

### 10.6 Failure behavior

Memory pressure never degrades correctness; it degrades retention. The actual chain on a refused Tier-3 reservation:

```text
refuse the reservation (never charge first)
    -> evict a zero-benefit Tier-3 category and retry the reservation
    -> stop requesting retainable answers for the rest of the batch
       (the traversal continues with exact evaluation, uncached)
```

Independently, a transaction the journal had to coarsen widens its impact region to the document and is evaluated exactly there. Whole-document exact evaluation runs as one pass; its scratch is charged to Tier 4, which is a reported ceiling rather than a refusable cap (§10.2).

A memory refusal can therefore make a flush slower and colder, but never rejects a synchronous style read as a Web-visible outcome.

## 11. Scheduling

Execution is sequential. Style transactions are processed as dependency-ordered stages over homogeneous delta queues, not recursive pointer-chasing calls. The flush (`take_style_transaction`, flush.rs) runs:

```text
1.  Normalize the journal into one transaction.
2.  Commit staged tree and local-feature deltas into the fact store
    (before routing, or after planning when the plan needs the before side).
3.  Decide reuse: does the transaction preserve selector incidence, reach no
    selector, or qualify for retained-answer patching or exact-cascade stops?
4.  Build impact regions (with a preorder topology above a size threshold).
5.  Route program and cascade-topology inputs FIRST (they set the outer
    envelope), then local-feature/tree/state inputs, stopping early once the
    region covers the document.
6.  Route sibling-sequence and relational (:has()) sequence changes; converge
    pending prefix routes.
7.  Widen for markers, normalize regions, resolve already-planned truths.
8.  Traverse: patch retained answers or complete published match answers;
    matched-rule production and winner-group updates happen together here,
    with exact-cascade stop and confirmation checks.
9.  Publish: build the reaction records and emit them in one callback.
```

Computed-value construction is C++-side: the reaction batch drives StyleComputer, which builds computed properties and publishes each element's computed groups back to the engine for interning (§13). Layout, paint, animation, and accessibility consequences are produced by the C++ reaction application.

No stage publishes script-observable state before the transaction commits: the emit callback only copies rows, and the FFI callback guard rejects reentry into the engine while it runs. Mutations made while reactions are applied are recorded as new input and become the next transaction of the same style-stabilization epoch; the update loop drains transactions until the document is stable. Old handles stay alive until the stabilization epoch and all its consumers retire.

**Priority** distinguishes latency-sensitive work from throughput work but never changes semantic transaction order:

* Interaction state and required style reads are latency-sensitive.
* Parser and stylesheet-load batches favor throughput.
* Background materialization and hot-query compilation are opportunistic.
* Memory reclamation can cancel speculative materialization.

An interaction transaction may run before uncommitted speculative compilation, but cannot pass an earlier stylesheet attachment that is already semantically committed. A forced style read waits for every preceding committed input delta and ignores later speculative work.

## 12. Consumers and laziness

Style is maintained for connected content, and every consuming subsystem reads it through the shared style handle:

| Consumer | Reads requiring current style |
| --- | --- |
| Layout | Box-tree construction, layout update, intrinsic sizing, fragmentation |
| Paint and compositing | Display-list construction, stacking, clipping, screenshots |
| Input and hit testing | Pointer events, cursor, hit-test visibility, native control behavior |
| Focus | Sequential focus navigation, focusability, focus-visible state |
| Find-in-page and selection | Visibility and generated-content participation |
| Accessibility | Tree creation, role/state updates, generated content, visibility |
| Animations | Base style, interpolation inputs, timeline attachment |
| View transitions | Old/new capture, generated `::view-transition*` pseudo-tree style, lifecycle snapshots |
| Script APIs | Resolved style, geometry APIs, selector APIs where relevant |
| Observers | Resize, intersection, media-query, and other delivery requiring updated style/layout |
| Display locking / `content-visibility` | Activation, skipped-content validation, retained state |
| Browser UI and automation | Printing, capture, WebDriver, DevTools inspection |

Recomputation reaches exactly the elements the reaction batch names, so laziness comes primarily from precision: an element whose inputs did not semantically change receives no reaction. Beyond that, two `display: none` cutoffs bound work in hidden content: the batch path does not propagate style recomputation into a `display: none` element's descendants (they carry only inherited-context reactions and are re-entered when an ancestor becomes visible), and the targeted read path stops its inheritance-chain walk at the topmost `display: none` ancestor. These cutoffs are derived from the current style at traversal time; a broader certificate-of-unobservation mechanism is a known future direction, not something the engine infers today.

Web-platform synchronization holds throughout: a style read flushes all preceding input transactions and returns a value from the resulting committed style state.

## 13. C++/Rust boundary

LibWeb C++ remains authoritative for DOM and CSSOM object identity, mutation semantics, document lifecycle, loading order, style-observation barriers, layout, and paint.

StyleEngine lives in the existing LibWeb Rust crate and is authoritative for selector matching, delta routing, cascade winners, and retained style state:

* the compact logical selector/program representation;
* physical state keyed by document-local StyleEngine IDs;
* the exact cold evaluator and the incremental evaluator;
* selector matching, cascade winner resolution, and reaction planning;
* match-answer identities, winner groups, memory accounting, and instrumentation.

Computed-value **construction** is C++-side: the reaction batch drives StyleComputer, which computes properties and builds computed-value groups, then publishes each element's groups back to the engine, where records are interned and become the shared authoritative representation. So the split is: Rust decides *what* must recompute and *which declarations win*; C++ computes *values*; Rust retains and interns the published result. C++ holds no copy of the engine's retained state; a fact has exactly one authoritative home.

**Identity allocation.** The engine mints `StyleNodeID` values (C++ requests them through an allocation call that accepts a batch); C++ owns DOM lifecycle and the DOM-to-style-node mapping. Rust treats IDs as opaque dense indexes. Reuse safety comes from two-phase retirement: retired indexes enter a pending pool and are released for reallocation only at a later safe boundary, never while a stale handle can observe them.

The engine likewise mints document-local `StyleAtomID` values for selector-mentioned tag, ID, class, attribute, value, namespace, and state identities, keyed by the exact interned-string identity C++ passes (a `Utf16FlyString`'s one-word raw identity, kept alive by the bridge for the atom's lifetime, so identity equality is exact and never hash-approximate). Interning on the engine side is what makes selector-name atoms and fact atoms comparable: one table assigns one sequence. Rust stores only the `u32` atom in compact bytecode and postings; a small number of text classes the engine must inspect byte-wise (attribute-value text for substring operators, language tags) are pushed to engine-side storage on demand and re-pushed if evicted.

**Relation columns** are stored on the Rust side, keyed by `StyleNodeID`, and maintained from the journalled tree deltas within the same transaction that reports the mutation, so transpose traversal, impact-region membership, and sibling-sequence scans run entirely inside the evaluator. Two deliberate exceptions bypass per-node journalling: initial bulk load links the whole arriving tree directly (safe because the root arrival forces whole-document evaluation), and shadow host/root registration is applied directly at registration time.

**Forward transfer.** Element-fact input crosses as one flat immutable transaction per flush: five pointer-free fixed-width row arrays (tree relations, local features, state, element declarations, element style inputs) submitted in a single call. Program, sheet, rule, layer, and topology changes cross as individual generated boundary calls as they happen and are journalled engine-side into the same normalization journal. A small set of per-element scalar facts (namespace, language, directionality, heading level, custom states, slot-ness) also cross as individual calls at mutation time; the design intent remains that per-element chatter is the exception, not the shape of the boundary.

**There is no reverse fact-fetch protocol.** The engine never asks C++ for a fact it lacks: C++ pushes every fact it owns eagerly as deltas, the engine stages them, and evaluation reads old/new values through a transaction-local fact view over the pending transaction plus the resident fact store. When a broad plan needs a complete fact table, the engine clones its own resident store (counted, charged to scratch): a Rust-to-Rust copy, no boundary round trip. The one reverse direction that exists is an enumeration callback letting C++ walk flat-tree descendants the engine knows about.

**Computed style crosses as a handle.** The boundary transfers a shared style-group handle, never a copied per-property object. C++ consumers hold `StyleRecordID` and read through the record view; layout and paint consume the same handle rather than materializing a private complete style per element (anonymous layout-derived boxes are the exception and own their values). Records are pinned while any C++ consumer references them and reclaimed per the Tier-1 rules in §10.1.

### 13.1 Module layout

The C++ side of the boundary is `Libraries/LibWeb/CSS/StyleEngineBridge.*` (the FFI surface; regular boundary calls, their recording frames, and replay decoders are generated from a single boundary specification at build time) and `Libraries/LibWeb/CSS/StyleEngineInput.*` (input collection and transaction assembly). The engine lives in `Libraries/LibWeb/Rust/src/css/style/`:

```text
mod.rs                engine root: modes, top-level state, public entry points
transaction.rs        normalization journal, typed deltas, epochs
transaction_view.rs   staged-input views over a pending transaction
inputs.rs             applying staged tree/fact input
program.rs            stylesheet program, identities, order tokens
program_updates.rs    ProgramDelta application
compiler.rs           selector compilation into the engine's programs
selector.rs, selector/  logical IR, match + transpose bytecode
relative_selector.rs  RelativeExists queries and inverse anchor enumeration
input_routing.rs      typed input key -> routing keys
routing.rs            delta routing, impact-region construction
impact.rs             impact regions and fail-closed coverage
planning.rs           transaction planning, selective-plan cutoff
index.rs              feature atoms, postings, per-node fact store
exact_matcher.rs      the exact cold evaluator (the reference implementation)
batch_matcher.rs      batched candidate matching schedules
matching.rs           match-answer production and completion
prefix.rs             prefix automaton: retained selector-prefix truths
ordering.rs           cascade ordering, match compaction
cascade.rs            priority, winner groups, winner keys
specified_value.rs    specified-value interning
computed.rs           computed-record group sets and publication storage
publication.rs        publishing computed state and reactions
flush.rs              flush orchestration: take_style_transaction
catalog.rs            match-answer catalog and answer caches
partial_view.rs       partial-materialization bookkeeping
intern_table.rs       the generic intern table (id + payload + lifetime policy)
sorted_merge.rs       the shared sorted delta-merge iterator
capacity.rs           declarative capacity accounting and guards
memory.rs             tiers, budget controller, eviction
order.rs              hierarchical order-maintenance tokens
tree.rs               style-node tree relations and identities
fast_hash.rs          deterministic hashing aliases (fixed-seed foldhash)
instrumentation.rs    counters and the wild ledger
record_replay.rs      capture/replay event stream (see StyleEngineTesting.md)
bridge.rs             Rust side of the generated FFI boundary
tests.rs, differential_tests.rs  in-crate unit and differential suites
```

## 14. Hard cases the implementation must handle

These defeat the sharing and index assumptions and must work by design, not by luck.

**A new selector has no useful rejecting feature.** A dynamically inserted broad universal/structural selector has no posting that rejects most of the scope. The candidate estimate crosses the selective cutoff, so it runs once through the exact batch evaluator over `Nscope`. If all its declarations lose, the transaction reports broad discovery cost and zero propagation. Existing selector truths are not invalidated; missing candidate state is reconstructed explicitly; no unchanged computed style is republished.

**Many winners change to the same value.** A root toggled from `theme-a` to `theme-b` with 50,000 `.item` descendants, where two descendant rules exchange matches and both declare the same literal canonical `color`. Declaration identity changes on every item, but declaration identity is not in `SpecifiedWinnerKey`; the canonical specified value and the (empty) dynamic read set are unchanged. Expected work shape:

| Stage | Count |
| --- | ---: |
| Normalized input | 1 class edit → 2 local feature deltas |
| Impact subjects | 50,000, plus charged routing and relation discovery |
| Exact selector checks | 100,000 (two selector programs) |
| Selector-truth deltas | 50,000 removals + 50,000 additions |
| Rule-match deltas | 50,000 removals + 50,000 additions |
| Winning declaration identity changes | 50,000 |
| `SpecifiedWinnerKey` changes | 0 |
| Computed `color` evaluations | 0 |
| Computed record identity, layout, paint changes | 0 |

The engine still charges the substantial selector and cascade discovery: this transaction is not free, it is *downstream*-free. If a fixture changes transition semantics or a dynamic read-set input, the appropriate key changes and resolution runs. Byte equality alone is never the stopping rule.

**Slot assignment and flat-tree relations change.** Moving a node between slots changes slot assignment and flat-tree identities even when the DOM parent is unchanged. `::slotted()`, host-context, inheritance, and any operator whose read set includes the affected relation are reevaluated in the shadow scope *and* the participating light-tree scope. Unrelated document-tree selectors keep their results. Never reuse a DOM-tree proof for a flat-tree question.

**Sibling-relative `:has()` churn.** For `.item:has(+ .item.selected)`, changing `selected` has exactly one possible anchor: the immediately preceding sibling. Direct relation evaluation is exact and allocates no summary. `.card:has(~ .panel .error)` can force a large sibling-and-descendant search: run the complex evaluator until the replacement budget is reached, then finish the query family with the exact batch plan. Boolean truth is never approximated.

**Tier-3 state disappears during interaction.** A backgrounded document loses postings, witnesses, and prefix caches. A later state change plus synchronous style read flushes the transaction, then uses DOM facts and the exact cold evaluator for the observed scope. The result must equal a never-evicted run. Returning to the foreground reconstructs nothing eagerly.

**Privacy-sensitive state.** When visited-dependent styling is implemented, its taint component must join operator, cache, and style-record identities, and instrumentation must redact tainted identities and never expose match cardinality to script or developer-facing timing counters. The requirement is recorded here so no cache added in the meantime bakes in an assumption that all results are shareable across a future privacy boundary.

## 15. Cost model

```text
Nscope       style nodes in the exact affected scope
Nlive        observed style nodes in that scope
Naffected    style nodes in the exact mutation-affected region
Nregion      style nodes enumerated by a typed impact-region stream
Ncandidates  candidates activated by one local input delta
Nsiblings    sibling positions whose selector truth may change
Sscope       selector operators attached to that scope
P            candidates in the narrowest usable posting
Qroute       transpose entry points routed from the changed typed input
Csel         exact candidate-check cost for the selector
Rsteps       parent/sibling relation steps performed by conventional combinators
Rimpact      relation positions visited while constructing the impact region
Hregion      relation steps to prove one posting candidate belongs to the region
             (executed against resident relation columns, no boundary crossing)
Aprefix      transaction-local selector-prefix work at one node
Mrule        concrete matches of one rule
Dchanged     declarations affected by a program edit
K            candidates inspected during witness replacement
Edepth       custom-property parent-environment depth
Tsub         token/function work on chosen substitution paths
```

Accepted bounds when no useful optional materialization exists:

| Operation | Discovery and repair bound |
| --- | --- |
| Local feature/state mutation | `O(Qroute + Rimpact + Ncandidates * Csel + Rsteps)`; routing never scans all `Sscope` headers |
| Intersect a posting with a subtree region | Selective `O(P * Hregion)` plus candidate checks, or batch `O(Nregion)` local-fact stream |
| Cold ordinary style pass | One style-node stream plus candidate program probes and `O(sum(Csel + Rsteps))` |
| Conventional ancestor mutation | `O(Naffected * Aprefix)`, stopping at unchanged selector outputs |
| Adjacent-sibling mutation | `O(1)` relation discovery per selector plus compound checks |
| Subsequent-sibling or positional mutation | `O(Nsiblings * Csel)` |
| Create a simple child/adjacent `:has()` witness | `O(Qroute + Csel)` per routed query before outer propagation |
| Create a simple descendant `:has()` witness | `O(Qroute + depth * Csel)` |
| Create a simple following-sibling `:has()` witness | `O(Qroute + preceding_siblings * Csel)` |
| Insert a selector with a rejecting posting | `O(P * (Hregion + Csel))` plus emitted matches, until the batch cutoff |
| Insert a selector with no rejecting index | `O(Nscope * Csel)` |
| Delete a cold rule | `O(Nscope * Csel)` via old-selector evaluation |
| Edit declarations of a cold rule | Same match discovery as deletion, then `O(Mrule * Dchanged)` cascade comparisons |
| Replace a sheet when structural diff fails | Exact batch of the old/new program delta; worst case `O(Nscope * Sscope * Csel)` |
| Replace one retained simple `:has()` witness | Forward walk of the query's axis, `O(K * Csel)` |
| Answer a cold complex `:has()` query | Exact traversal of its anchor region; worst case `O(Nscope * Csel)` per query family |
| Resolve a cold custom property | `O(Edepth + Tsub)` including cycle handling |
| Tier-3 miss for an observed node | Exact selector and cascade evaluation for that scope; worst case `O(Nscope * Sscope * Csel)` |
| Broad source-order or root inherited change | `O(Nlive)` or greater when that many outputs change |

Accepted lower bounds the design does not pretend to beat:

* A new selector may require inspecting every element its indexes cannot reject.
* A source-order change can truly change a property on every matching element.
* A structural mutation can truly change positional results for many siblings.
* A root inherited-value change can truly affect every observed descendant.

The engine's job is to make broad costs correspond to real semantic uncertainty or real output, instrument them, and stop claiming selectivity when a batch plan would be cheaper.

## 16. Instrumentation

The engine maintains a large counter ledger (instrumentation.rs; ~160 counters), exposed to C++ and through the internals object, alongside a separate C++-side ledger of style-update and invalidation counters. The main families:

* Input deltas by kind; journal cancellations, replacements, and coarsened scope markers.
* Executed selector primitives in five buckets: local-feature tests, state tests, combinator steps, structural tests, relational tests.
* Routing: routed entry points, impact relation steps, region cardinality.
* Candidate enumeration: posting entries enumerated, region-membership checks, exact checks, rejections.
* Matching and stopping: retained-answer patch stops, cascade compaction counts, cold-batch rows evaluated and missing.
* Tier-3 memory decisions: refusals per category, admission retries, benefit evictions.
* Bytes by tier and category, plus the pressure snapshot (limits, refusals, evictions) exposed over FFI and printed by the replay accounting report.
* Style records and interned states created, reused, reclaimed; reaction and recomputation counts on the C++ ledger (the amplification triage numbers).

The doctrine over any future counter: ratios with a zero denominator report the raw numerator and `not applicable`, never an invented zero, and every plan decision made against an estimate should leave behind the observation that would have corrected it.

## 17. Failure modes and safeguards

*Database counterpart:* these are the database's own pathologies in this engine's terms: view-size blowup, trigger fanout, buffer-pool thrashing, plan instability under churn, stale statistics. Each safeguard is the standard remedy, stated concretely.

**Materialization explosion.** A selector family may generate unique states for most elements. Refuse the allocation at the Tier-3 limit and use exact cold evaluation. Low sharing is a real failure mode, not something hidden by calling the objects interned.

**Dependency-edge explosion.** Never allocate an unbounded reverse subscriber list. When a direct edge budget would be crossed, retain no list and validate the exact affected scope from shared input identities.

**Match-relation duplication.** Canonicalize match answers while a traversal consumes them. Per-node retained match answers and per-program selector incidences exist only as Tier-3 evictable views (never required state, never a complete `RuleID -> elements` relation), so the duplication is bounded by the budget and disappears under pressure.

**Cache thrashing.** With no promotion logic, repeated cold reconstruction is directly visible in counters. Any later promotion mechanism must use hysteresis, rebuild cost, and a re-promotion penalty.

**Adversarial stylesheet churn.** Program transactions normalize repeated edits and reuse immutable IR. Per-origin and per-scope work queues stop one churning sheet from forcing unrelated selector reevaluation.

**Pathological broad changes.** When a change produces huge output, batch it. Never pretend the output can be avoided and never add unrelated invalidation work.

**Stale lazy state.** Every lazy result records the identities of its semantic inputs or a shared context token. Observation validates those identities. Eviction and physical compaction never modify semantic identities.

**Identifier exhaustion.** Document-local IDs are never reused while stale handles may exist (two-phase retirement). Exhaustion of the 32-bit index space is treated as unreachable for one document; if that assumption ever breaks, the answer is an arena rebuild at a safe boundary, never silent aliasing.

## 18. Verification

The engine's correctness claim (every path produces results identical to full recomputation from authoritative inputs) is enforced by standing machinery, not by convention:

* **The exact cold evaluator is the reference implementation.** It is plain by design, free of the incremental path's caches, and is both the eviction fallback and the oracle every other path is checked against.
* **In-crate unit and differential suites** (`tests.rs`, `differential_tests.rs`) exercise operators directly, including differential comparison of incremental against cold evaluation.
* **Verify modes** re-derive incremental answers through the cold path at runtime and compare, observer-only: verification never mutates engine state, never disables the fast path it checks, and treats an incomplete comparison as a failure.
* **Record/replay** captures the complete engine input stream of a real browsing session and replays it deterministically; digests over published outputs make any divergence, down to one changed identity, a hard failure.

Commands, workflows, and debugging guidance for all of these are in [StyleEngineTesting.md](StyleEngineTesting.md).

