// Test that generator yield with non-trivial argument expression produces
// correct bytecode: completion registers must be allocated before evaluating
// the argument expression, matching C++ register allocation order.

function* f(a, b) {
    yield a.x;
    yield b.y;
}
f({ x: 1 }, { y: 2 });
