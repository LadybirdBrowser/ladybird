// Compiled code depends on things it no longer checks (prototype chains, global declarations, global bindings never
// assigned, stable shapes); the runtime invalidates it when one stops holding, also while frames run it, which then
// exit where they next rely on it. And calls made by inlined callees run without the frames of the inlined calls,
// which stack walks, exceptions and legacy properties must still see. Each case breaks a dependency or looks at the
// frames from inside such a call, while the compiled frame is on the stack.

test("a prototype changes while compiled code runs", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const prototype = {
                get value() {
                    return 1;
                },
            };
            const mutate = change => {
                if (change) Object.defineProperty(prototype, "value", { get: () => 2, configurable: true });
            };
            return change => {
                const object = Object.create(prototype);
                const before = object.value;
                mutate(change);
                return [before, object.value, Object.create(prototype).value];
            };
        },
        { warmUp: () => [false], probes: { change: () => [true], again: () => [false] } }
    );
    expect(failures).toEqual([]);
});

test("a method on a prototype is replaced while compiled code runs", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            class Point {
                constructor(x) {
                    this.x = x;
                }
                size() {
                    return this.x;
                }
            }
            const replace = yes => {
                if (yes)
                    Point.prototype.size = function () {
                        return -this.x;
                    };
            };
            return (x, yes) => {
                const point = new Point(x);
                const first = point.size();
                replace(yes);
                return [first, point.size()];
            };
        },
        { warmUp: (log, i) => [i, false], probes: { replaced: () => [5, true], after: () => [6, false] } }
    );
    expect(failures).toEqual([]);
});

test("an object leaves a stable shape while compiled code runs", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const objects = [];
            for (let i = 0; i < 4; ++i) objects.push({ a: i, b: i + 1 });
            const reshape = index => {
                if (index >= 0) {
                    delete objects[index].a;
                    objects[index].c = "new";
                }
            };
            return index => {
                let sum = 0;
                for (const object of objects) sum += object.b;
                reshape(index);
                for (const object of objects) sum += object.b + (object.a ?? 100);
                return sum;
            };
        },
        { warmUp: () => [-1], probes: { reshaped: () => [2], "reshaped again": () => [0] } }
    );
    expect(failures).toEqual([]);
});

test("a global binding is assigned after compiled code assumed it never is", () => {
    // NB: The binding is shared by both functions the comparison runs, so it is assigned the value it has.
    try {
        evaluateSource("let jitInvalidationBinding = 7;");
    } catch {}
    const failures = compareCompiledWithInterpreter(
        () => assign => {
            const before = jitInvalidationBinding;
            if (assign) evaluateSource("jitInvalidationBinding = 7;");
            return [before, jitInvalidationBinding + 1];
        },
        { warmUp: () => [false], probes: { assigned: () => [true], again: () => [false] } }
    );
    expect(failures).toEqual([]);
});

test("exceptions thrown by calls in inlined callees", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function deep(value) {
                if (value > 5) throw new RangeError(`deep ${value}`);
                return value;
            }
            function notInlined(value) {
                let result = value;
                for (let i = 0; i < 3; ++i) result = (result + i) | 0;
                return deep(result);
            }
            const inlined = value => notInlined(value) + 1;
            return (value, log) => {
                try {
                    return inlined(value);
                } catch (e) {
                    log.push(e.message);
                    return "caught";
                }
            };
        },
        { warmUp: (log, i) => [i % 3, log], probes: { throwing: log => [9, log], string: log => ["1", log] } }
    );
    expect(failures).toEqual([]);
});

test("stack traces taken in calls of inlined callees", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function where() {
                return new Error("here").stack.split("\n").length;
            }
            const inner = value => where() + value;
            const middle = value => inner(value) * 2;
            return value => middle(value);
        },
        { warmUp: (log, i) => [i], probes: { double: () => [0.5], string: () => ["x"] } }
    );
    expect(failures).toEqual([]);
});

test("re-entering compiled code from calls of inlined callees", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            let self;
            const callBack = depth => (depth > 0 ? self(depth - 1) : 0);
            const inlined = depth => callBack(depth) + 1;
            self = depth => inlined(depth);
            return depth => self(depth);
        },
        { warmUp: (log, i) => [i % 4], probes: { deeper: () => [20], string: () => ["3"] } }
    );
    expect(failures).toEqual([]);
});
