// Test that a named function expression only affects identifiers inside the
// function body, not identifiers outside. The outer Oops should use GetGlobal.

Oops = function Oops() {
    Oops;
};

Oops.x;
