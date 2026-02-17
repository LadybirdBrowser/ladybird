// Test that var-declared locals do not get ThrowIfTDZ on assignment,
// even when used before the var declaration (hoisted).

function isect() {
    for (i = 0; i < 3; i++) {}
    for (var i = 0; i < 3; i++) {}
}

isect();
