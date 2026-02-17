// Test that assigning to a function parameter does not emit ThrowIfTDZ,
// since parameters are never in the temporal dead zone.

function isect(far, d) {
    far = d;
}

isect();
