// Callers in a stack trace are shown at their call sites, also when the interpreter takes its fast paths for
// calls that it has made before.

function callerFrame(stack, name) {
    const frame = stack.split("\n").find(line => line.includes(`at ${name} (`));
    return frame.match(/:(\d+:\d+)\)$/)[1];
}

function inner() {
    return new Error().stack;
}

function callsInner() {
    let unrelated = 1;
    unrelated += 1;
    return inner();
}

function callsGetter() {
    const object = {
        get value() {
            return new Error().stack;
        },
    };
    return object.value;
}

function needsEnvironment() {
    let captured = 1;
    const closure = () => captured;
    return new Error().stack;
}

function callsNeedsEnvironment() {
    let unrelated = 1;
    unrelated += 1;
    return needsEnvironment();
}

test("caller of a plain call", () => {
    for (let i = 0; i < 3; ++i) expect(callerFrame(callsInner(), "callsInner")).toBe("16:17");
});

test("caller of an inline getter call", () => {
    for (let i = 0; i < 3; ++i) expect(callerFrame(callsGetter(), "callsGetter")).toBe("25:18");
});

test("caller of a call to a function that needs an environment", () => {
    for (let i = 0; i < 3; ++i) expect(callerFrame(callsNeedsEnvironment(), "callsNeedsEnvironment")).toBe("37:28");
});
