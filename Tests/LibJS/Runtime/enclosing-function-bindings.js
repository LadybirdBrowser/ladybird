test("reads and writes bindings several functions up", () => {
    "use strict";
    function outer(a) {
        let b = a * 10;
        function middle() {
            let c = b + 1;
            return function inner() {
                a += 1;
                b += 1;
                return [a, b, c];
            };
        }
        return middle();
    }
    const inner = outer(1);
    expect(inner()).toEqual([2, 11, 11]);
    expect(inner()).toEqual([3, 12, 11]);
});

test("functions without an environment of their own", () => {
    function outer() {
        let value = 1;
        const passThrough = () => () => () => value++;
        return passThrough()();
    }
    const read = outer();
    expect(read()).toBe(1);
    expect(read()).toBe(2);
});

test("closures from separate calls assign to their own bindings", () => {
    function counter() {
        "use strict";
        let count = 0;
        return {
            increment() {
                count = count + 1;
            },
            get: () => count,
        };
    }
    const first = counter();
    const second = counter();
    for (let i = 0; i < 10; ++i) first.increment();
    for (let i = 0; i < 3; ++i) second.increment();
    expect(first.get()).toBe(10);
    expect(second.get()).toBe(3);
});

test("assignment before initialization throws after earlier assignments succeeded", () => {
    function scope(assignEarly) {
        "use strict";
        function assign(value) {
            binding = value;
        }
        if (assignEarly) {
            expect(() => assign(1)).toThrow(ReferenceError);
            return;
        }
        let binding = 0;
        for (let i = 0; i < 5; ++i) assign(i);
        expect(binding).toBe(4);
    }
    scope(false);
    scope(false);
    scope(true);
    scope(false);
});

test("assignment to a captured const keeps throwing", () => {
    "use strict";
    const constant = 1;
    const assign = () => {
        constant = 2;
    };
    for (let i = 0; i < 3; ++i) expect(assign).toThrow(TypeError);
    expect(constant).toBe(1);
});

test("assignment to an undeclared name keeps throwing", () => {
    function assign() {
        "use strict";
        undeclaredStrictAssignmentTarget = 1;
    }
    for (let i = 0; i < 3; ++i) expect(assign).toThrow(ReferenceError);
    expect(globalThis.hasOwnProperty("undeclaredStrictAssignmentTarget")).toBeFalse();
});

test("assignment keeps the binding it resolved before its right-hand side ran", () => {
    let readShadowed;
    function scope() {
        var target = "initial";
        let generator;
        function* shadowing() {
            yield function (resume) {
                "use strict";
                target = (resume && (readShadowed = generator.next().value), "assigned");
            };
            eval("var target = 'shadowed'");
            yield () => target;
        }
        generator = shadowing();
        const assign = generator.next().value;
        assign(false);
        assign(false);
        target = "initial";
        assign(true);
        return target;
    }
    expect(scope()).toBe("assigned");
    expect(readShadowed()).toBe("shadowed");
});

test("assignment whose right-hand side yields", () => {
    function scope() {
        "use strict";
        let target = "initial";
        function* assign() {
            target = yield;
        }
        return { assign, read: () => target };
    }
    const first = scope();
    const second = scope();
    const suspended = first.assign();
    suspended.next();
    for (let i = 0; i < 3; ++i) {
        const generator = second.assign();
        generator.next();
        generator.next(i);
    }
    suspended.next("resumed");
    expect(first.read()).toBe("resumed");
    expect(second.read()).toBe(2);
});

test("bindings of blocks, catch clauses and loops several scopes up", () => {
    "use strict";
    let total = 0;
    const closures = [];
    for (let i = 0; i < 3; ++i) {
        let latest = -1;
        try {
            throw i;
        } catch (caught) {
            {
                let inner = caught * 10;
                closures.push({
                    assign() {
                        total = total + 1;
                        latest = inner + i;
                    },
                    read: () => latest,
                });
            }
        }
    }
    for (let round = 0; round < 3; ++round) {
        for (const closure of closures) closure.assign();
    }
    expect(total).toBe(9);
    expect(closures.map(closure => closure.read())).toEqual([0, 11, 22]);
});

test("named function expressions", () => {
    function outer() {
        let calls = 0;
        const recurse = function count(n) {
            calls++;
            return n === 0 ? count : count(n - 1);
        };
        const assignStrict = function name() {
            "use strict";
            name = 1;
        };
        const assignNonStrict = function name() {
            name = 1;
            return name;
        };
        return { recurse, assignStrict, assignNonStrict, calls: () => calls };
    }
    const { recurse, assignStrict, assignNonStrict, calls } = outer();
    expect(recurse(3)).toBe(recurse);
    expect(calls()).toBe(4);
    expect(assignStrict).toThrow(TypeError);
    expect(assignNonStrict()).toBe(assignNonStrict);
});

test("class elements", () => {
    function outer() {
        let base = 1;
        class C {
            static fromField = base + 1;
            field = () => base + C.fromField;
            static {
                this.fromBlock = base + 2;
            }
            method() {
                return base + C.fromBlock;
            }
            get accessor() {
                return C.name + base;
            }
        }
        base = 10;
        return new C();
    }
    const instance = outer();
    expect(instance.field()).toBe(12);
    expect(instance.method()).toBe(13);
    expect(instance.accessor).toBe("C10");
});

test("parameter default closures", () => {
    function outer() {
        let x = "outer";
        function f(a = () => x, b = () => a) {
            var x = "body";
            return [a(), b()(), x];
        }
        return f();
    }
    expect(outer()).toEqual(["outer", "outer", "body"]);
});

test("generators and async functions after resuming", () => {
    function outer() {
        let count = 0;
        function* generator() {
            count++;
            yield count;
            count++;
            yield count;
        }
        async function asynchronous() {
            await null;
            return ++count;
        }
        return { generator, asynchronous };
    }
    const { generator, asynchronous } = outer();
    expect([...generator()]).toEqual([1, 2]);
    let result;
    asynchronous().then(value => {
        result = value;
    });
    runQueuedPromiseJobs();
    expect(result).toBe(3);
});

test("Annex B block functions", () => {
    function outer() {
        let captured = "outer";
        {
            function hoisted() {
                return captured;
            }
        }
        return () => hoisted();
    }
    expect(outer()()).toBe("outer");
});

test("duplicate block function declarations", () => {
    function inBlock() {
        {
            function target() {
                return "first";
            }
            function read() {
                return target();
            }
            function target() {
                return "second";
            }
            return read();
        }
    }
    function inSwitch() {
        switch (0) {
            case 0:
                function target() {
                    return "first";
                }
                function read() {
                    return target();
                }
            default:
                function target() {
                    return "second";
                }
                return read();
        }
    }
    function assignment() {
        {
            function target() {
                return "first";
            }
            function write() {
                target = () => "written";
            }
            function target() {
                return "second";
            }
            write();
            return target();
        }
    }
    expect(inBlock()).toBe("second");
    expect(inSwitch()).toBe("second");
    expect(assignment()).toBe("written");
});

test("non-strict direct eval in an enclosing function", () => {
    function outer() {
        let x = "outer";
        function middle(code) {
            eval(code);
            return () => x;
        }
        return [middle("")(), middle("var x = 'eval'")()];
    }
    expect(outer()).toEqual(["outer", "eval"]);
});

test("strict direct eval in an enclosing function", () => {
    function outer() {
        let x = "outer";
        function middle() {
            "use strict";
            eval("var x = 'eval'");
            return () => x;
        }
        return middle()();
    }
    expect(outer()).toBe("outer");
});

test("functions created by eval code", () => {
    function outer() {
        let x = "outer";
        return eval("(function () { let y = 'eval'; return () => x + y; })()");
    }
    expect(outer()()).toBe("outereval");
});

test("with statement between a closure and the binding", () => {
    function outer(object) {
        let value = "outer";
        with (object) {
            return () => value;
        }
    }
    expect(outer({})()).toBe("outer");
    expect(outer({ value: "object" })()).toBe("object");
});

test("assignment through a with statement's object", () => {
    function scope(object) {
        var value = "outer";
        with (object) {
            var assign = function () {
                "use strict";
                value = "assigned";
            };
        }
        for (let i = 0; i < 3; ++i) assign();
        return value;
    }
    const object = { value: "object" };
    expect(scope({})).toBe("assigned");
    expect(scope(object)).toBe("outer");
    expect(object.value).toBe("assigned");
});
