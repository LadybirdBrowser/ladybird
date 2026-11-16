function frameLines(stack) {
    return stack.split("\n").filter(line => line.startsWith("    at "));
}

function recurse(depth, callback) {
    return depth > 0 ? recurse(depth - 1, callback) : callback();
}

test("is a function of Error with length 2", () => {
    expect(Error.captureStackTrace).toHaveLength(2);
    expect(Error.captureStackTrace.name).toBe("captureStackTrace");
    const descriptor = Object.getOwnPropertyDescriptor(Error, "captureStackTrace");
    expect(descriptor.writable).toBeTrue();
    expect(descriptor.enumerable).toBeFalse();
    expect(descriptor.configurable).toBeTrue();
    expect(Error.captureStackTrace({})).toBeUndefined();
});

test("defines an own, writable, configurable and not enumerable data property", () => {
    const object = {};
    Error.captureStackTrace(object);
    const descriptor = Object.getOwnPropertyDescriptor(object, "stack");
    expect(typeof descriptor.value).toBe("string");
    expect(descriptor.writable).toBeTrue();
    expect(descriptor.enumerable).toBeFalse();
    expect(descriptor.configurable).toBeTrue();
    expect(Object.keys(object)).toEqual([]);
});

test("the stack starts with the name and message of the object, then the frames of the caller", () => {
    function captureIn(object) {
        Error.captureStackTrace(object);
        return object.stack;
    }
    const lines = captureIn({}).split("\n");
    expect(lines[0]).toBe("Error");
    expect(lines[1].startsWith("    at captureIn (")).toBeTrue();
    expect(captureIn({ name: "Custom", message: "text" }).split("\n")[0]).toBe("Custom: text");
    expect(captureIn({ message: "text" }).split("\n")[0]).toBe("Error: text");
    expect(captureIn({ name: "", message: "text" }).split("\n")[0]).toBe("text");
    expect(captureIn({ name: "Named", message: "" }).split("\n")[0]).toBe("Named");
    expect(captureIn({ toString: () => "ignored" }).split("\n")[0]).toBe("Error");

    const error = new TypeError("first");
    error.message = "second";
    expect(captureIn(error).split("\n")[0]).toBe("TypeError: second");
});

test("the string is made at once", () => {
    const object = { message: "before" };
    Error.captureStackTrace(object);
    object.message = "after";
    expect(object.stack.split("\n")[0]).toBe("Error: before");

    const throwing = {
        get name() {
            throw new Error("from name");
        },
    };
    expect(() => Error.captureStackTrace(throwing)).toThrowWithMessage(Error, "from name");
    expect(Object.hasOwn(throwing, "stack")).toBeFalse();
});

test("frames up to and including the function are left out", () => {
    const object = {};
    function inner() {
        Error.captureStackTrace(object, middle);
    }
    function middle() {
        inner();
    }
    function outer() {
        middle();
    }
    outer();
    const frames = frameLines(object.stack);
    expect(frames[0].startsWith("    at outer (")).toBeTrue();
});

test("the innermost frame of a recursive function is the one left out", () => {
    const object = {};
    function recursive(depth) {
        if (depth > 0) return recursive(depth - 1);
        Error.captureStackTrace(object, recursive);
    }
    recursive(3);
    const frames = frameLines(object.stack);
    expect(frames[0].startsWith("    at recursive (")).toBeTrue();
    expect(frames[3].startsWith("    at recursive (")).toBeFalse();
    expect(frames[2].startsWith("    at recursive (")).toBeTrue();
});

test("a function that is not on the stack leaves out every frame", () => {
    const object = {};
    function notCalled() {}
    Error.captureStackTrace(object, notCalled);
    expect(object.stack).toBe("Error\n");
});

test("a value that is not a function, a bound function and a proxy leave out only the frame of captureStackTrace", () => {
    function capturer(fn) {
        const object = {};
        Error.captureStackTrace(object, fn);
        return frameLines(object.stack)[0];
    }
    for (const fn of [undefined, 5, {}, capturer.bind(null), new Proxy(capturer, {})])
        expect(capturer(fn).startsWith("    at capturer (")).toBeTrue();
});

test("the stack takes Error.stackTraceLimit", () => {
    const saved = Error.stackTraceLimit;
    try {
        Error.stackTraceLimit = 3;
        const object = {};
        recurse(20, () => Error.captureStackTrace(object));
        expect(frameLines(object.stack)).toHaveLength(3);

        Error.stackTraceLimit = 0;
        recurse(20, () => Error.captureStackTrace(object));
        expect(object.stack).toBe("Error\n");

        Error.stackTraceLimit = "x";
        recurse(20, () => Error.captureStackTrace(object));
        expect(Object.hasOwn(object, "stack")).toBeTrue();
        expect(object.stack).toBeUndefined();
    } finally {
        Error.stackTraceLimit = saved;
    }
});

test("replaces a configurable stack property", () => {
    const object = { stack: "old" };
    Error.captureStackTrace(object);
    expect(object.stack.startsWith("Error\n")).toBeTrue();
    expect(Object.keys(object)).toEqual([]);

    const error = new Error("own");
    Error.captureStackTrace(error);
    expect(Object.hasOwn(error, "stack")).toBeTrue();
    expect(error.stack.startsWith("Error: own\n")).toBeTrue();
});

test("errors", () => {
    for (const value of [undefined, null, 1, "string", Symbol.iterator, 1n, true]) {
        expect(() => Error.captureStackTrace(value)).toThrow(TypeError);
    }

    const frozen = Object.freeze({});
    expect(() => Error.captureStackTrace(frozen)).toThrowWithMessage(
        TypeError,
        "Cannot define property stack, object is not extensible"
    );
    const notExtensible = Object.preventExtensions({ stack: "kept" });
    expect(() => Error.captureStackTrace(notExtensible)).toThrow(TypeError);
    expect(notExtensible.stack).toBe("kept");

    const nonConfigurable = {};
    Object.defineProperty(nonConfigurable, "stack", { value: "fixed" });
    expect(() => Error.captureStackTrace(nonConfigurable)).toThrow(TypeError);
    expect(nonConfigurable.stack).toBe("fixed");
});
