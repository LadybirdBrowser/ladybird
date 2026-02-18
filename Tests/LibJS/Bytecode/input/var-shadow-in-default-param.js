// A body `var` that shadows a name referenced in a default parameter
// expression must resolve the name from the outer scope at runtime,
// not from the (uninitialized) body var binding.

// A body `var` that shadows a name used in a default -- must use GetBinding,
// and a separate variable environment must be created.
var shadow = "outer";
function shadow_in_default(x = shadow) {
    var shadow = "inner";
    return x;
}
shadow_in_default();

// No conflict -- body var `y` should stay a local (Mov, no GetBinding).
function no_conflict(x = 1) {
    var y = 2;
    return y;
}
no_conflict();
