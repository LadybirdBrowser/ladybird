# The style engine

LibWeb's style engine lives in `Libraries/LibWeb/Rust/src/css/style/`. It works like a conventional browser style engine:
it matches an element against an index of rules, cascades what matched, and computes values into groups that elements
share. What sets it apart is where it runs. A document's style state belongs to the render side, on the Render
thread, and the main thread hands it inputs and takes back results. That lets the render clock sample animations and hover
while script runs, without ever reading the DOM.

[StyleEngineTesting.md](StyleEngineTesting.md) covers how to test and debug it.

## Inputs

**The projection** (`projection.rs`) is the engine's copy of what style reads of the DOM: one row per element with its tag,
namespace and id atoms, classes, states, the attribute values selectors read, its inline and presentational declaration
blocks, and its links to its parent and siblings. The main thread writes it at mutation time (`CSS/StyleEngineInput.cpp`,
`projection_writes.rs`). Rows live in copy-on-write columns, so a generation of them is immutable plain data during a pass,
and a fork of the engine shares every chunk it does not write.

Input the engine cannot use is not sent. An attribute no selector or `attr()` reads is withheld, and the elements carrying it
are noted and published if something comes to read the name. `:local-link` works the same way.

**The rule store** (`rule_store.rs`) is the semantic program of the stylesheets, separate from the CSSOM objects, which
keep API identity and serialization. A parsed sheet is shared between the documents that link it, and a document keeps only
what it must number for itself.

## A style pass

**The rule index** (`rules.rs`) buckets the entries of each tree scope by the most selective key the subject compound requires,
each with the keys its ancestors must carry. A change indexes only the rules it names. Shadow trees with the same sheets and
layers share one group of entries.

**The traversal** (`traversal.rs`) styles every element once its parent is styled, breadth-first from the root, on a pool of
threads once a level is wide enough (`LIBWEB_STYLE_THREADS` sets the number). Each thread keeps its own matcher, cascade
scratch, ancestor Bloom filter and caches.

For each element it:

1. Tries to keep the previous record or take one from the sharing cache (`sharing.rs`), without matching.
2. Matches (`matching.rs`): gathers the bucket candidates, rejects through the Bloom filter, and evaluates the rest.
3. Cascades (`cascade.rs`): orders declarations by priority, and the first of each longhand wins.
4. Computes (`compute.rs`, `drive.rs`): starts from the parent's inherited groups and the default reset groups as pointer copies,
   and writes only the groups its winners touch. Most values were precomputed when the rule was published, and matched lists
   that recur reuse their groups through the group cache.

**Records** (`records.rs`) hold the result: the groups of computed values, the facts a reader needs without opening them, and
what the computation read, which later changes are tested against. A record's slot and generation identify it to the main thread.

## Restyling

After a document's first pass, each style update restyles only what its inputs can reach (`live.rs`):

**Invalidation** (`invalidation.rs`) maps every fact a compound tests to where that compound stands in its selector. A changed
element restyles itself where the fact is the subject's, and otherwise follows the combinators to the elements that can match.
`:has()` compounds find their anchors through their own queries. Reads that carry no structure, such as the selectors of an
`of` or the bounds of a `@scope`, restyle coarsely.

A restyled element's children restyle only where what they inherit changed. Custom properties move without a restyle
where a child substitutes none that moved (`custom_property_moves.rs`).

Whole-document restyles are not a tool here. A change that reaches everything is a bug to find.

**Style on demand** (`demand.rs`) answers a read no pass keeps current, such as the style below a `display: none` element. It is
computed read-only into an overlay, kept until a record it depends on could move.

## Installation

`install.rs` turns the records that moved into rows, and the render side installs them into the layout arena's style columns.
The main thread takes the same rows at the end of a style update (`CSS/UpdateStyle.cpp`) and does what depends on the DOM:
starting transitions and animations, applying the effects `main_effects.rs` decided, and invalidating layout.

An update can fly: it runs beside the event loop, and the next update takes in its results once it has landed. Script reading
style or geometry in the meantime waits for it.

## The render clock

Animations are sampled on the Render thread, composing overlays over records (`engine_sample.rs`). A pointer move hovers
there too (`hover_lane.rs`, `render_state/clock/hover.rs`): it writes the hover facts and restyles what they reach while
the main thread runs a task. A frame where no element can match a compound testing `:hover` goes read-only and needs only a hit test.

## Memory and instrumentation

`internals.styleMemoryReport()` reports what the engine holds by owner, and the budgets under
`Tests/LibWeb/Text/input/css/style-memory/` hold it to what it should cost. `internals.styleCounters()` and
`internals.styleEngineCounters()` report the work done, and `Meta/StyleUpdateCapture/` records it for a page.
