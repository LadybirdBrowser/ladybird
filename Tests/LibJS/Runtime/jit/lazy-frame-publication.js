// Direct calls of compiled functions leave the callee's frame unpublished, and its code publishes it before anything
// else may see it: before calls, allocations and slow paths. Exits, and callees without compiled code, publish it in
// the runtime.
//
// NB: The callees are too large to inline, so that compiled callers call them directly. Their first parameter only
//     pads them, and is never passed.

function makeCallee(parameters, body) {
    return new Function("pad", ...parameters, `if (pad === 1) { ${"pad = pad + 1; ".repeat(16)}} ${body}`);
}

function compileCall(caller, callee, warmUp) {
    jit.prepare(callee);
    jit.prepare(caller);
    for (let i = 0; i < 10; ++i) warmUp(i);
    jit.compile(callee);
    jit.compile(caller);
}

test("exits from callees that did not publish their frame", () => {
    const increment = makeCallee(["value"], "return value + 1;");
    const callIncrement = value => increment(undefined, value);
    compileCall(callIncrement, increment, i => expect(callIncrement(i)).toBe(i + 1));
    expect(callIncrement(5)).toBe(6);
    expect(callIncrement("a")).toBe("a1");
    expect(callIncrement(2147483647)).toBe(2147483648);
});

test("allocations in callees keep their arguments alive", () => {
    const box = makeCallee(["value"], "return { value };");
    const callBox = value => box(undefined, value);
    compileCall(callBox, box, i => expect(callBox(i).value).toBe(i));
    for (let i = 0; i < 20000; ++i) {
        const value = { i };
        const boxed = callBox(value);
        expect(boxed.value).toBe(value);
        expect(boxed.value.i).toBe(i);
    }
    gc();
    expect(callBox("kept").value).toBe("kept");
});

test("callees that call out show in stack traces", () => {
    const stackOf = makeCallee([], "return new Error().stack;");
    const callStackOf = () => stackOf();
    compileCall(callStackOf, stackOf, i => callStackOf(i));
    const stack = callStackOf();
    expect(stack.includes("at anonymous (")).toBeTrue();
    expect(stack.includes("at callStackOf (")).toBeTrue();
});

test("callees without compiled code run in the interpreter", () => {
    const add = makeCallee(["a", "b"], "return a + b;");
    const callAdd = (a, b) => add(undefined, a, b);
    if (jit.enabled) jit.neverCompile(add);
    jit.prepare(callAdd);
    for (let i = 0; i < 10; ++i) expect(callAdd(i, 1)).toBe(i + 1);
    jit.compile(callAdd);
    expect(callAdd(2, 3)).toBe(5);
    expect(callAdd("a", "b")).toBe("ab");
});

test("callees that run out of stack at their entry", () => {
    const depth = n => (n === 0 ? 0 : 1 + depth(n - 1));
    jit.prepare(depth);
    for (let i = 0; i < 10; ++i) expect(depth(i)).toBe(i);
    jit.compile(depth);
    expect(depth(1000)).toBe(1000);
    expect(() => depth(10000000)).toThrow();
    expect(depth(100)).toBe(100);
});
