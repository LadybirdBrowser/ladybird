// `arguments` as a non-computed property key is just a name token,
// not a reference. The function should NOT be marked might-need-arguments.
function plain_arguments_key() {
    return { arguments: 1 };
}

// Same for `eval` as a non-computed property key.
function plain_eval_key() {
    return { eval: 1 };
}

// FIXME: Shorthand `{ arguments }` IS a reference to the binding and
// should mark the function might-need-arguments. The expected output
// here currently captures the still-buggy behavior — the function's
// arguments register is allocated but never initialized, which causes
// a runtime crash if the property is read. A proper fix needs to also
// handle the cover-grammar reinterpretation in `({ arguments } = ...)`.
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
