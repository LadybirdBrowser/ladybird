// Test that returning from inside a with statement correctly emits
// SetLexicalEnvironment to restore the parent environment before Return.

function with_return(o) {
    with (o) {
        return x;
    }
}

with_return({ x: 42 });
