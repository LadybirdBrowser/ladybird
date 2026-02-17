// Test that generator yield expressions produce correct bytecode:
// - Correct register reuse across multiple yields
// - No unnecessary register copies on the normal path
// - Proper completion protocol (Normal/Throw/Return dispatch)

function* multi_yield() {
    yield 1;
    yield 2;
}

var g = multi_yield();
g.next();
g.next();
