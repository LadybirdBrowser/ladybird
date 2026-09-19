// Every scenario runs several times so that later runs reuse the environment shape recorded by the
// first run.

test("named function expressions", () => {
    function makeCounter() {
        return function counter(n) {
            return n === 0 ? 0 : 1 + counter(n - 1);
        };
    }
    for (let i = 0; i < 5; ++i) {
        const counter = makeCounter();
        expect(counter(3)).toBe(3);
        expect(counter.name).toBe("counter");
    }
});

test("named function expression name binding is immutable", () => {
    function makeSloppy() {
        return function f() {
            f = 1;
            return typeof f;
        };
    }
    function makeStrict() {
        return function g() {
            "use strict";
            g = 1;
        };
    }
    for (let i = 0; i < 5; ++i) {
        expect(makeSloppy()()).toBe("function");
        expect(() => makeStrict()()).toThrow(TypeError);
    }
});

test("per-iteration for loop bindings", () => {
    function collect() {
        const closures = [];
        for (let i = 0, j = 10; i < 3; ++i, ++j) {
            closures.push(() => [i, j]);
        }
        return closures.map(closure => closure());
    }
    for (let run = 0; run < 5; ++run) {
        expect(collect()).toEqual([
            [0, 10],
            [1, 11],
            [2, 12],
        ]);
    }
});

test("for-of and for-in head declarations", () => {
    function collect(object) {
        const closures = [];
        for (const [key, value] of Object.entries(object)) {
            closures.push(() => key + value);
        }
        for (const key in object) {
            closures.push(() => key);
        }
        return closures.map(closure => closure());
    }
    for (let run = 0; run < 5; ++run) {
        expect(collect({ a: 1, b: 2 })).toEqual(["a1", "b2", "a", "b"]);
    }
});

test("named and anonymous class expressions", () => {
    function makeNamed() {
        return class Named {
            static self() {
                return Named;
            }
        };
    }
    function makeAnonymous() {
        return class {
            static tag() {
                return "anonymous";
            }
        };
    }
    for (let run = 0; run < 5; ++run) {
        const Named = makeNamed();
        expect(Named.self()).toBe(Named);
        expect(Named.name).toBe("Named");
        expect(makeAnonymous().tag()).toBe("anonymous");
    }
});

test("parameter scope with parameter expressions", () => {
    function f(a, b = () => a, c = () => b) {
        var a = "shadowed";
        return [b(), c() === b, a];
    }
    for (let run = 0; run < 5; ++run) {
        expect(f(run)).toEqual([run, true, "shadowed"]);
    }
});

test("catch parameter environment", () => {
    function capture(value) {
        try {
            throw value;
        } catch (error) {
            return () => error;
        }
    }
    for (let run = 0; run < 5; ++run) {
        expect(capture(run)()).toBe(run);
    }
});

test("block and switch scopes", () => {
    function blocks(n) {
        const closures = [];
        {
            let a = n;
            const b = n + 1;
            function declared() {
                return a + b;
            }
            closures.push(() => [a, b, declared()]);
        }
        switch (n) {
            case n: {
                let c = n * 2;
                closures.push(() => c);
            }
        }
        return closures.map(closure => closure());
    }
    for (let run = 0; run < 5; ++run) {
        expect(blocks(run)).toEqual([[run, run + 1, run * 2 + 1], run * 2]);
    }
});

test("temporal dead zone still applies with a cached shape", () => {
    function tdz() {
        const closures = [];
        for (let i = 0; i < 1; ++i) {
            closures.push(() => later);
            let later = i;
        }
        return closures[0]();
    }
    for (let run = 0; run < 5; ++run) {
        expect(tdz()).toBe(0);
    }
    function early() {
        {
            const read = () => value;
            read();
            const value = 1;
        }
    }
    for (let run = 0; run < 5; ++run) {
        expect(early).toThrow(ReferenceError);
    }
});
