// Test that nested for-loops do not emit redundant completion Movs
// from the parser-generated scope wrapper blocks.

for (var n = 4; n <= 7; n += 1) {
    for (var depth = 0; depth <= 0; depth += 2) {}
}
