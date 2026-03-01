// Test that assigning to a function argument does NOT emit a ThrowIfTDZ
// instruction. Arguments are always initialized, so TDZ checks are
// unnecessary.

function f(x) {
    let y = 1;
    x = y;
    return x;
}

f(0);
