// Test that the for-in completion register is initialized before the
// nullish check that skips the iteration.

1;
for (var a in null) {
}
