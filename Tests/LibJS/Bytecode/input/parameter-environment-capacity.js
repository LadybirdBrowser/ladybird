// Test that environments get room for exactly the bindings created in them
// when parameters have their own environment because of parameter expressions:
// the parameters, the arguments object and, in strict code without var
// declarations of its own, the lexical declarations.

function sloppy(a = 1) {
    return () => a + arguments.length;
}

class Strict {
    method(mode = "x", index) {
        const active = mode === "a";
        return () => mode + index + active;
    }
}

sloppy();
new Strict().method();
