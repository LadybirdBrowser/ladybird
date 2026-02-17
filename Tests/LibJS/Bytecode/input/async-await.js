// Test that async/await expressions produce correct bytecode:
// - AwaitExpression saves accumulator to received_completion before await
// - ReturnStatement's implicit await allocates completion registers externally
// - Register allocation matches between explicit and implicit await paths

async function f() {
    return await Promise.resolve(42);
}
f();
