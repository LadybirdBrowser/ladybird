// Rest-only parameter should get [argument:0].
function rest_only(...args) {
    return args.length;
}

// Rest-only parameter should still get [argument:0] even when
// the function also accesses the arguments object.
function rest_with_arguments(...args) {
    return args.length + arguments.length;
}
