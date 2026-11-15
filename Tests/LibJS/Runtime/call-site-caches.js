// A call site remembers the function it called last, with what a call of it needs. Calls through the same site have
// to behave as if nothing were remembered.

describe("call site caches", () => {
    test("closures of one function keep their own environments", () => {
        function makeCounter(start) {
            let count = start;
            return () => ++count;
        }
        function callIt(f) {
            return f();
        }
        const first = makeCounter(0);
        const second = makeCounter(100);
        for (let i = 1; i <= 5; ++i) {
            expect(callIt(first)).toBe(i);
            expect(callIt(second)).toBe(100 + i);
        }
    });

    test("one site that calls functions and native functions in turn", () => {
        function callIt(f, value) {
            return f(value);
        }
        const twice = value => value * 2;
        for (let i = 0; i < 5; ++i) {
            expect(callIt(Math.abs, -i)).toBe(i);
            expect(callIt(twice, i)).toBe(i * 2);
            expect(callIt(String, i)).toBe(String(i));
        }
    });

    test("a callee that died, and a function that came after it", () => {
        function callIt(f) {
            return f();
        }
        function make(value) {
            return function () {
                return value;
            };
        }
        for (let i = 0; i < 20; ++i) {
            expect(callIt(make(i))).toBe(i);
            gc();
        }
    });

    test("sloppy functions see the global object as this for every receiver", () => {
        function sloppy() {
            return this;
        }
        function strict() {
            "use strict";
            return this;
        }
        function callIt(f, receiver) {
            return f.call(receiver);
        }
        function callPlain(f) {
            return f();
        }
        for (let i = 0; i < 3; ++i) {
            expect(callPlain(sloppy)).toBe(globalThis);
            expect(callPlain(strict)).toBeUndefined();
            expect(typeof callIt(sloppy, 1)).toBe("object");
            expect(callIt(strict, 1)).toBe(1);
        }
    });

    test("a class constructor called without new throws every time", () => {
        class C {}
        function callIt(f) {
            return f();
        }
        function plain() {
            return 1;
        }
        expect(callIt(plain)).toBe(1);
        for (let i = 0; i < 3; ++i) {
            expect(() => callIt(C)).toThrowWithMessage(TypeError, "must be called with 'new'");
            expect(callIt(plain)).toBe(1);
        }
    });

    // NB: As in code-flushing.js, the body runs once and never in JIT code, which would hold the code of the callee.
    let flushedTestRan = false;
    test("a callee whose code was flushed compiles again", function run() {
        if (flushedTestRan) return;
        flushedTestRan = true;
        function add(a, b) {
            return a + b;
        }
        function callIt(a, b) {
            return add(a, b);
        }
        if (typeof jit !== "undefined") {
            jit.neverCompile(run);
            jit.neverCompile(callIt);
        }
        expect(callIt(1, 2)).toBe(3);
        flushCode();
        expect(isCodeFlushed(add)).toBeTrue();
        expect(callIt(3, 4)).toBe(7);
        expect(callIt(5, 6)).toBe(11);
    });
});

// NB: The callee is only reachable through a slot that the collector rewrites, so a collection that moves cells moves
//     it, and the call site never runs in JIT code, which would keep the callee where it is.
function callThroughSite(f) {
    return f();
}
function makeReturning(value) {
    return function () {
        return value;
    };
}
// NB: The functions that may take the address of the callee that moved have other code, which a call through an entry
//     that still had the old address would not run.
function makeDoubling(value) {
    return function () {
        return value * 2;
    };
}
if (typeof jit !== "undefined") jit.neverCompile(callThroughSite);
const movingCallee = { f: makeReturning("moved") };
for (let i = 0; i < 20; ++i) callThroughSite(movingCallee.f);
const movingCalleeAddress = addressOf(movingCallee.f);

runAfterMovingCollection(() => {
    test("a callee that moved, and a function that took its address", () => {
        expect(addressOf(movingCallee.f)).not.toBe(movingCalleeAddress);
        const functions = [];
        for (let i = 0; i < 2000; ++i) {
            const f = makeDoubling(i);
            functions.push(f);
            if (addressOf(f) === movingCalleeAddress) {
                expect(callThroughSite(f)).toBe(i * 2);
                break;
            }
        }
        expect(callThroughSite(movingCallee.f)).toBe("moved");
    });
});
