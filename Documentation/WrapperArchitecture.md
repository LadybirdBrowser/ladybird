# LibWeb wrapper architecture

LibWeb exposes internal C++ implementation objects to JavaScript through a
three-layer wrapper model:

1. `Web::Bindings::Wrappable` is the implementation-side base for objects that
   can be reflected into JavaScript.
2. The WebIDL generator emits `PlatformObject` wrapper classes in
   `Web::Bindings`. These wrappers hold the implementation object and implement
   the Web-facing object behavior.
3. `Web::Bindings::WrapperWorld` owns wrapper identity for one observable world.
   Wrapping an implementation object means looking up or creating the wrapper in
   the caller's `WrapperWorld`.

This gives LibWeb one wrapper identity per `(implementation object,
WrapperWorld)` pair, while still allowing the same implementation object to be
observed through different realms or isolated worlds.

## World model

Each HTML agent owns one main-world `WrapperWorld`. For windows this means one
cell per similar-origin window agent, not per `Page`: same-agent auxiliary
pages such as same-origin `window.open()` can directly observe each other's DOM
objects and must therefore share main-world wrapper identity. Worker agents
naturally get their own main-world cell. Cross-origin windows in one WebContent
process currently share its `SimilarOriginWindowAgent`, so they also share this
cell; proper agent-cluster separation is future work (including the same-process
COOP/`noopener` popup case noted in `PageClient.cpp`).

Non-main worlds are realm-local wrapper-cache cells. The current concrete types
are `Internal` and `Extension`; each has its own wrapper cache and must not
share main-world slots. Non-main worlds that may preserve wrappers must be
explicitly detached before the embedder drops them. The destructor verifies
this unconditionally, except during `CollectEverything` collection where weak
wrappable references cannot be walked safely. Wrapping, preserving, or cache
insertion through a detached world is likewise rejected by unconditional
`VERIFY`s, including in release builds.

`Wrappable` keeps an inline weak main-world wrapper as a fast-path
representative, but the authoritative cache is the `WrapperWorld` map. This is
important because the same implementation object can be visible through
different realms in the same agent without splitting identity. A main-world
wrapper cache hit from a different main-world cell is treated as an invariant
violation; cross-agent object graphs are not directly shared.

## Realm rule

Generated bindings make one realm decision per IDL member. Instance members
default to the validated receiver realm; static members, constructors, and
namespace members use the caller/current realm; `[NeedsCallerRealm]` is the
single opt-out for instance members whose observable allocation realm is the
caller realm.

The detailed policy, including exception wrapping and current opt-out
categories, lives in
[REALM_POLICY.md](../Meta/Generators/libweb_bindings/REALM_POLICY.md).

`WindowProxy` has one special receiver rule: for main-world proxies,
`Bindings::this_value_realm()` resolves to the active `[[Window]]` realm; for
non-main-world proxies, it stays in the proxy's own realm.

## Realms for DOM developers

Implementation code should not mention realms unless the algorithm is
explicitly binding-adjacent or JS-object-materialization code. Bindings choose
the observable realm for exceptions, return values, callbacks, and ordinary
wrapper creation.

For day-to-day DOM implementation work, realm decisions should fit in this
contract:

> Implementation code never mentions realms. Bindings choose realms. You think
> about global ownership in exactly two situations: (1) a non-`Node` wrappable
> may be observed from several globals before it is wrapped — override
> `relevant_global_impl()` and return the owning `Window`, `Document`'s window,
> or worker global-scope implementation; (2) you mint a `Promise` for later
> resolution — use the promise vending helper for the relevant object or
> settings object. Everything else — exceptions, return values, events,
> callbacks — is realm-free in ordinary implementation code.

Existing implementation-side realm mentions are transitional unless they are
needed for spec-selected JS object materialization, stream/chunk conversion,
structured serialization, module/script plumbing, or callback invocation. New
realm mentions outside `Bindings/`,
`HTML/Scripting/`, and `WebIDL/` should be treated as architecture changes and
must be justified in the lint allowlist.

## Preservation and wrapper state

Any non-default JavaScript state on a platform wrapper preserves that exact
wrapper. This includes:

- ordinary, accessor, and symbol expandos;
- expando creation through inline-cache paths;
- subclass construction state;
- custom prototypes;
- non-default extensibility/frozen/sealed state.

The C++ inline-cache preservation trigger is
`JS::Object::Flag::RequiresSlowAddOwnProperty`: platform wrappers set this flag
so fast expando writes route through the preserving add-own-property path.

Preservation is intentionally append-only for a wrapper. Deleting an expando
does not unpreserve the wrapper; tests for deletion should assert the deleted
property stays deleted after GC, not that the wrapper becomes collectable.

Tests should use `withCollectedWrapper(makeAndMark, reacquire, verify)` from
`Tests/LibWeb/Text/input/include.js` when checking preserved-wrapper behavior.
That helper verifies `internals.wrapperIsPreserved()` before forcing GC, drops
the local wrapper reference, then re-acquires through the implementation object.

## Liveness model

Ordinary wrapper/implementation edges are:

- wrapper to implementation: strong, generated wrapper member;
- implementation to cached wrapper: weak, through `WrapperWorld` and the
  inline main-world representative;
- implementation to preserved wrapper: strong, visited by `Wrappable`.

Pending activity is explicit. Objects such as timers, XHR, WebSocket, and
ResizeObserver take and release `GC::ActivityRoot`s at their state transitions
instead of relying on silent GC virtual predicates.

LibGC finalization clears wrapper cache entries before weak pointers are
observed as live again. Code that depends on weak wrapper caches relies on that
ordering: wrapper finalization removes the cache entry, then weak references can
clear.

## Window and Location cross-origin descriptors

Cross-origin property descriptor maps live on the per-world wrapper, not on the
implementation object. `WindowWrapper` and `LocationWrapper` both follow this
pattern. Descriptor cache reachability is ordinary wrapper reachability: cached
descriptor values and accessors are traced through the wrapper, but they are not
roots, so a wrapper/realm cycle can still be collected when nothing outside the
cycle reaches it. This removes the need for world serials or detach-time pruning
of implementation-owned maps.

Each world's wrapper has its own descriptor map. A descriptor cached for one
world cannot alias another world's descriptor because it is stored in a
different wrapper object.

## Accepted risks

- Adopted nodes may keep using a wrapper from their original main-world realm
  when that wrapper was already preserved. This is accepted because clearing the
  edge would make wrapper identity depend on navigation/bfcache timing.
- Preservation can over-retain wrappers after state returns to default, such as
  after deleting an expando. This is accepted because append-only preservation is
  simple, deterministic, and avoids a fragile "all wrapper state is default"
  detector.
- A non-`Node` implementation without a relevant-global override falls back to
  the caller's preferred realm. If its unpreserved wrapper dies, a replacement
  wrapper may therefore be allocated in a different realm in the same world.
  Wrapper-world identity remains unchanged; implementations with an override
  derive stable placement from their owning global implementation.
- Cross-origin descriptor cache keys use raw environment-settings-object
  addresses. This is accepted because the key is only used inside a descriptor
  map owned by one live wrapper, and entries are not reused after the wrapper
  dies. The raw addresses are identity tokens for same-lifetime cache lookup,
  not owning references.

## Future implementation representation

The binding layer must not assume implementation objects are permanently
GC-allocated. The expected long-term state is heterogeneous: some implementations
may remain GC cells, some may become C++ refcounted objects, and some may become
Rust-backed handles.

Keep new wrapper work behind the narrow binding surface:

- `wrap()`;
- `impl_from<T>()` / `wrappable_impl()`;
- `preserve_wrapper()`;
- `WrapperWorld` caches;
- the generated root wrapper's implementation member.

Do not add ad-hoc implementation-pointer plumbing or new GC-specific liveness
side channels outside that surface. When non-GC implementation objects arrive,
the migration points are the generated implementation handle, weak
implementation-to-wrapper caches, preserved-wrapper roots, and the current
`ActivityRoot` model.

## Testing wrapper and GC behavior (mandatory rules)

These rules exist because early wrapper work produced ~10 regression tests
that could not fail and two whose checked-in expectations encoded the bug they
were meant to catch. They are hard requirements for any test touching wrapper
identity, preservation, realms, or GC:

1. **Drop and re-acquire.** A wrapper held in a live local across
   `internals.gc()` can never be collected, so the test asserts nothing.
   Create/mutate inside an IIFE (or null the variable), `gc()`, then
   re-acquire through the implementation (`getElementById`, collection
   access) and assert on the re-acquired object. Prefer
   `withCollectedWrapper()` from `Tests/LibWeb/Text/input/include.js`.
   Holding a ref is only correct when the test's point is that a held ref
   pins identity — say so in a comment.
2. **`Function` takes (params..., body).** `Function("x => f(x)")` makes the
   string the function *body* of a zero-argument function. For cross-realm
   calls use `frame.contentWindow.Function("arg", "return arg.item(0)")` and
   verify the result is not `undefined`.
3. **Read every expectation file before committing it.** If the expected
   output contains `false`, `undefined`, or an unchanged "before" value,
   either the test records a failure or it is an intentional negative
   assertion — if the latter, say so in the test.
4. **Prove the test can fail.** For every regression test guarding a fix,
   revert the fix locally, watch the test fail, restore, and record the
   failure text in the change description. A test that passes both with and
   without the fix is a coverage pin — label it as such.
5. **Never mark work done with an unmet acceptance criterion** — record an
   explicit deviation instead. When several test failures share a signature,
   diagnose the shared cause before writing per-site fixes; a green gate is a
   necessary condition, never the goal.
