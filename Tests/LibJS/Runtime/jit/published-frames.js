// Slow paths and calls in inlined callees run in the frames of the inlined calls, which the runtime publishes with
// their headers only: their function, `this`, arguments and position. Frames that do not continue are written in
// full.

function callsValue(value) {
    return value();
}

function callsCallsValue(value) {
    return callsValue(value);
}

const callees = [() => 1, () => 2, () => 3, () => 4];

test("calls that throw in inlined callees show every frame", () => {
    for (let i = 0; i < 200; ++i) expect(callsCallsValue(callees[i % callees.length])).toBe((i % callees.length) + 1);
    let stack = "";
    try {
        callsCallsValue(5);
    } catch (error) {
        stack = error.stack;
    }
    expect(stack.includes("at callsValue (")).toBeTrue();
    expect(stack.includes("at callsCallsValue (")).toBeTrue();
});

test("calls in inlined callees see their callers at their call sites", () => {
    const stackOfCaller = () => new Error().stack;
    for (let i = 0; i < 200; ++i) {
        const stack = callsCallsValue(i % 2 ? stackOfCaller : () => new Error().stack);
        expect(stack.includes("at callsValue (")).toBeTrue();
        expect(/at callsCallsValue \(.*:10:\d+\)/.test(stack)).toBeTrue();
    }
});
