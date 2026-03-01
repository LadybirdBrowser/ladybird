// Test that simple destructuring parameters (without default values or
// nested expressions) do NOT create a separate parameter environment.
// The non-local binding should use CreateVariable directly in the
// function environment, not in a new CreateLexicalEnvironment.

function f({ captured, local }) {
    return () => captured;
}

f({ captured: 1, local: 2 });
