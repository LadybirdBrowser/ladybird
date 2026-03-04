// Test that simple assignment to a let variable emits ThrowIfTDZ
// before the store, ensuring TDZ semantics are enforced.

function g(x) { return x; }

function f(n) {
    let { location: a } = n;
    "string" == typeof a && (a = g(a));
    return a;
}

f({ location: "hello" });
