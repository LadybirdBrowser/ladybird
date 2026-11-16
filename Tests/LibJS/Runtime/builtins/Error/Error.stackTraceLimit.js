function recurse(depth, callback) {
    return depth > 0 ? recurse(depth - 1, callback) : callback();
}

function frameLines(stack) {
    return stack.split("\n").filter(line => line.startsWith("    at "));
}

function withStackTraceLimit(limit, callback) {
    const saved = Object.getOwnPropertyDescriptor(Error, "stackTraceLimit");
    if (limit === undefined) delete Error.stackTraceLimit;
    else Error.stackTraceLimit = limit;
    try {
        return callback();
    } finally {
        Object.defineProperty(Error, "stackTraceLimit", saved);
    }
}

function stackAtDepth(depth, limit, makeError = () => new Error("deep")) {
    return withStackTraceLimit(limit, () => recurse(depth, makeError).stack);
}

test("is a writable, enumerable and configurable data property with the value 10", () => {
    expect(Object.hasOwn(Error, "stackTraceLimit")).toBeTrue();
    const descriptor = Object.getOwnPropertyDescriptor(Error, "stackTraceLimit");
    expect(descriptor.value).toBe(10);
    expect(descriptor.writable).toBeTrue();
    expect(descriptor.enumerable).toBeTrue();
    expect(descriptor.configurable).toBeTrue();
    expect(Object.hasOwn(TypeError, "stackTraceLimit")).toBeFalse();
});

test("errors keep 10 frames by default", () => {
    const stack = recurse(30, () => new Error("deep")).stack;
    expect(stack.split("\n")[0]).toBe("Error: deep");
    const frames = frameLines(stack);
    expect(frames).toHaveLength(10);
    expect(frames[0].includes("Error.stackTraceLimit.js:")).toBeTrue();
    expect(frames[1].includes("at recurse (")).toBeTrue();
});

test("numbers limit the frames", () => {
    expect(frameLines(stackAtDepth(30, 3))).toHaveLength(3);
    expect(frameLines(stackAtDepth(30, 3.9))).toHaveLength(3);
    expect(frameLines(stackAtDepth(30, 25))).toHaveLength(25);
    expect(frameLines(stackAtDepth(30, 1))[0].includes("Error.stackTraceLimit.js:")).toBeTrue();
});

test("zero, negative numbers and NaN keep no frames", () => {
    for (const limit of [0, -0, -1, -Infinity, NaN]) {
        const stack = stackAtDepth(30, limit);
        expect(stack.split("\n")[0]).toBe("Error: deep");
        expect(frameLines(stack)).toHaveLength(0);
    }
});

test("Infinity keeps every frame", () => {
    const deep = frameLines(stackAtDepth(200, Infinity));
    expect(deep.length).toBeGreaterThan(200);
    expect(frameLines(stackAtDepth(10, Infinity)).length).toBe(deep.length - 190);
});

test("a stack shorter than the limit is not cut", () => {
    // NB: Every stack is taken from the same line, so that their frames are the same.
    let all;
    const stacks = [];
    for (const extra of [null, 0, 1, -1]) {
        const frames = frameLines(stackAtDepth(0, extra === null ? Infinity : all.length + extra));
        if (extra === null) all = frames;
        else stacks.push(frames);
    }
    const [sameAsCount, oneMore, oneLess] = stacks;
    expect(all.length).toBeLessThan(100);
    expect(sameAsCount).toEqual(all);
    expect(oneMore).toEqual(all);
    expect(oneLess).toEqual(all.slice(0, -1));
});

test("a value that is not a number captures no stack", () => {
    for (const limit of ["5", {}, null, true, 5n, Symbol.iterator, undefined]) {
        expect(stackAtDepth(5, limit)).toBeUndefined();
        expect(withStackTraceLimit(limit, () => recurse(5, () => new TypeError("t")))).toBeInstanceOf(TypeError);
    }
    // NB: A stack captured with a limit is kept when the limit changes.
    const error = withStackTraceLimit(2, () => new Error());
    expect(withStackTraceLimit("x", () => frameLines(error.stack))).toHaveLength(2);
});

test("deleting the property captures no stack", () => {
    let error;
    withStackTraceLimit(5, () => {
        delete Error.stackTraceLimit;
        error = new Error("gone");
    });
    expect(error.stack).toBeUndefined();
    expect(error.message).toBe("gone");
});

test("an accessor is not a number, and its getter does not run", () => {
    let getterCalls = 0;
    const saved = Object.getOwnPropertyDescriptor(Error, "stackTraceLimit");
    Object.defineProperty(Error, "stackTraceLimit", {
        get() {
            getterCalls++;
            return 3;
        },
        configurable: true,
    });
    try {
        expect(new Error().stack).toBeUndefined();
    } finally {
        Object.defineProperty(Error, "stackTraceLimit", saved);
    }
    expect(getterCalls).toBe(0);
});

test("a value inherited by Error is not read", () => {
    const savedPrototype = Object.getPrototypeOf(Error);
    Object.setPrototypeOf(Error, { stackTraceLimit: 2 });
    try {
        expect(stackAtDepth(5, undefined)).toBeUndefined();
    } finally {
        Object.setPrototypeOf(Error, savedPrototype);
    }
});

test("errors the engine makes take the limit", () => {
    const makeTypeError = () => {
        try {
            null.property;
        } catch (error) {
            return error;
        }
    };
    expect(frameLines(stackAtDepth(30, 4, makeTypeError))).toHaveLength(4);
    expect(stackAtDepth(30, "4", makeTypeError)).toBeUndefined();

    function overflow() {
        return overflow() + 1;
    }
    let rangeError;
    try {
        overflow();
    } catch (error) {
        rangeError = error;
    }
    // NB: The error of a stack overflow is an InternalError.
    expect(rangeError).toBeInstanceOf(Error);
    expect(frameLines(rangeError.stack).length).toBeLessThanOrEqual(10);
});

test("every error type takes the limit", () => {
    for (const ErrorType of [EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError, InternalError]) {
        expect(frameLines(stackAtDepth(30, 2, () => new ErrorType()))).toHaveLength(2);
    }
    expect(frameLines(stackAtDepth(30, 2, () => new AggregateError([])))).toHaveLength(2);
    expect(frameLines(stackAtDepth(30, 2, () => new SuppressedError(1, 2)))).toHaveLength(2);
});
