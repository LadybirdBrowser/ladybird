// Functions that jit.prepare() moved to the profiling handlers run alongside plain ones. The handlers switch dispatch
// tables whenever they continue in another frame, so every frame runs with the handlers of its own executable.

test("calls between profiled and plain functions", () => {
    function plainCallee(a, b) {
        return a * b;
    }
    function profiledCallee(a, b) {
        return a + b;
    }
    function profiledCaller(n) {
        let sum = 0;
        for (let i = 0; i < n; ++i) sum += plainCallee(i, 2) + profiledCallee(i, 1);
        return sum;
    }
    function plainCaller(n) {
        let sum = 0;
        for (let i = 0; i < n; ++i) sum += profiledCaller(i);
        return sum;
    }
    jit.prepare(profiledCallee);
    jit.prepare(profiledCaller);
    expect(profiledCaller(10)).toBe(145);
    expect(plainCaller(5)).toBe(40);
});

test("exceptions thrown across profiled and plain frames", () => {
    function thrower(value) {
        if (value > 2) throw new Error(`too big: ${value}`);
        return value;
    }
    function profiledCatcher(value) {
        try {
            return thrower(value);
        } catch (error) {
            return error.message;
        }
    }
    function profiledPasser(value) {
        return thrower(value) + 1;
    }
    jit.prepare(profiledCatcher);
    jit.prepare(profiledPasser);
    expect(profiledCatcher(1)).toBe(1);
    expect(profiledCatcher(3)).toBe("too big: 3");
    expect(() => profiledPasser(4)).toThrowWithMessage(Error, "too big: 4");
    expect(profiledPasser(2)).toBe(3);
});

test("getters and constructors run as inline frames", () => {
    class Point {
        constructor(x, y) {
            this.x = x;
            this.y = y;
        }
        get length() {
            return Math.abs(this.x) + Math.abs(this.y);
        }
    }
    function make(x, y) {
        return new Point(x, y).length;
    }
    jit.prepare(make);
    jit.prepare(Point);
    expect(make(1, -2)).toBe(3);
    expect(make(-4, 5)).toBe(9);
});

test("generators and async functions resume with their own handlers", () => {
    function* counter(limit) {
        for (let i = 0; i < limit; ++i) yield i;
    }
    async function sum(values) {
        let total = 0;
        for (const value of values) total += await value;
        return total;
    }
    jit.prepare(counter);
    jit.prepare(sum);
    expect([...counter(4)]).toEqual([0, 1, 2, 3]);
    let result;
    sum([1, Promise.resolve(2), 3]).then(value => {
        result = value;
    });
    runQueuedPromiseJobs();
    expect(result).toBe(6);
});

test("only functions are prepared", () => {
    expect(() => jit.prepare(Math.max)).toThrowWithMessage(TypeError, "Not an ECMAScript function");
    expect(() => jit.prepare(1)).toThrowWithMessage(TypeError, "Not an ECMAScript function");
});
