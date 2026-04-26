// `arguments` as a non-computed property key is just a name token,
// not a reference. The function should NOT be marked might-need-arguments.
function plain_arguments_key() {
    return { arguments: 1 };
}

// Same for `eval` as a non-computed property key.
function plain_eval_key() {
    return { eval: 1 };
}

// Shorthand `{ arguments }` IS a reference to the binding. The parser
// doesn't fire its conservative might-need-arguments flag here (the
// shorthand path doesn't go through the eval/arguments check on
// consume), but scope analysis still allocates a local for `arguments`
// and the bytecode generator falls back to that signal so the
// arguments object IS materialized at runtime.
function shorthand_arguments() {
    return { arguments };
}

// Computed `[arguments]` is a real identifier reference inside the
// computed-key expression, so the function is marked might-need-arguments.
function computed_arguments_key() {
    return { [arguments]: 1 };
}

// Computed `[eval]` similarly.
function computed_eval_key() {
    return { [eval]: 1 };
}

// Binding-pattern non-computed key with name `arguments`. The key
// is a property name, not a reference; the alias `x` is the bound
// name. Should NOT mark might-need-arguments on its own.
function binding_pattern_arguments_key() {
    let { arguments: x } = { arguments: 1 };
    return x;
}

// Method named `arguments` is again a property-key context.
function method_named_arguments() {
    return { arguments() { return 1; } }.arguments();
}
