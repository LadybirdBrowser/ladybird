// Test that try/catch where all paths are terminated (return/throw)
// does not emit a spurious End instruction after the try body.

function try_return() {
    try {
        throw 1;
    } catch (e) {
        return e;
    }
}

try_return();
