# LibWeb binding realm policy

Generated bindings make one explicit realm decision per IDL member.

- Instance operations and attributes default to the validated receiver realm
  (`this_object_realm`). This realm is used for implementation calls,
  DOMException wrapping, JavaScript return-value wrapping, and promise
  creation/rejection for promise-typed members.
- Generated pair-iterable, async-iterable, maplike, and setlike helpers use the
  receiver realm for iterator/container creation and wrapped values.
- Static operations, constructors, and namespace members use the caller/current
  realm because there is no receiver.
- `[RealmFreeConstructor]` marks an `[ImplementedInBindings]` constructor whose
  custom binding helper does not need the current realm. The generated
  constructor still uses the caller/current realm for WebIDL conversions,
  exception wrapping, wrapper allocation, and prototype selection; only the
  implementation helper call omits the realm argument.
- `[NeedsCallerRealm]` is the single opt-out for an instance member whose
  Web-facing result or implementation algorithm is specified to use the
  caller/current realm. Getter/setter-specific caller-realm annotations are not
  used; an attribute-level `[NeedsCallerRealm]` applies to the generated
  accessor(s).

The shared generator entry point is
`Meta/Generators/libweb_bindings/realms.py::member_realm_expr()`. New generated
member code should use that helper instead of spelling ad-hoc realm policy in
individual emitters.

This deliberately deviates from WebIDL's default exception wording for
instance-member DOMException wrapping: LibWeb wraps DOMExceptions in the
receiver realm so wrapper allocation, exception identity, and return-value
allocation follow the same observable realm. Generated operation and setter
conversion blocks also scope ordinary JavaScript TypeError construction to the
selected member realm. The VM clears that TypeError override whenever
conversion re-enters author script, so nested script still throws in its own
realm. This is intentionally narrower than changing the VM's current realm or
execution context, so conversion failures get uniform realm identity without
running user script under a different realm.

Current `[NeedsCallerRealm]` opt-out categories:

- WebAssembly JS API objects and namespace functions: JS-API algorithms create
  ArrayBuffers, typed arrays, exports objects, and promises in the caller realm.
- Promise/body/fetch/cache/credential/permission/serial/gamepad/media APIs:
  promise capability and result object allocation is caller-observable.
- Structured serialization APIs (`postMessage`, `History`, `Navigation`
  state): cloning/deserialization is defined against the caller/current realm.
- APIs that consume caller JS callbacks/constructors or objects as policy
  inputs (`CustomElementRegistry`, Trusted Types, keyframes, CSS property
  registration).
- Explicit WPT/spec carve-outs kept as caller-realm behavior, including
  `SourceBuffer.buffered` and `AbortSignal.abort()`/`timeout()`.

For `WindowProxy` receivers, `this_object_realm` is provided by
`Bindings::this_value_realm()`: main-world proxies resolve to the active
`[[Window]]` realm, while non-main-world proxies stay in their own realm.
