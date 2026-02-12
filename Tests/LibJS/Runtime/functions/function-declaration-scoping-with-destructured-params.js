// This test covers a bug where function declarations inside functions with
// destructured parameters were not visible in nested closures. The issue was
// that CreateVariableEnvironment (which holds function declarations) was not
// properly chained as the parent of the subsequent lexical environment.

describe("function declarations accessible from closures with destructured object params", () => {
    test("basic: aliased destructured param with default", () => {
        function go({ tz: o = 420 }) {
            let s = 69;
            function y() {
                return s;
            }
            return {
                _foo: y,
                _bar: function () {
                    return y();
                },
            };
        }
        let result = go({});
        expect(result._foo()).toBe(69);
        expect(result._bar()).toBe(69);
    });

    test("simple destructured param, no default", () => {
        function go({ x }) {
            let val = 42;
            function inner() {
                return val + x;
            }
            return function () {
                return inner();
            };
        }
        expect(go({ x: 8 })()).toBe(50);
    });

    test("multiple destructured params", () => {
        function go({ a }, { b }) {
            function inner() {
                return a + b;
            }
            return function () {
                return inner();
            };
        }
        expect(go({ a: 10 }, { b: 20 })()).toBe(30);
    });

    test("destructured param with default value used", () => {
        function go({ x = 100 }) {
            function inner() {
                return x;
            }
            return function () {
                return inner();
            };
        }
        expect(go({})()).toBe(100);
        expect(go({ x: 5 })()).toBe(5);
    });

    test("destructured param with default value, plus let binding", () => {
        function go({ x = 100 }) {
            let y = x + 1;
            function inner() {
                return y;
            }
            return function () {
                return inner();
            };
        }
        expect(go({})()).toBe(101);
        expect(go({ x: 5 })()).toBe(6);
    });

    test("nested destructured param", () => {
        function go({ a: { b } }) {
            function inner() {
                return b;
            }
            return function () {
                return inner();
            };
        }
        expect(go({ a: { b: 99 } })()).toBe(99);
    });

    test("rest element in destructured param", () => {
        function go({ x, ...rest }) {
            function inner() {
                return rest.y;
            }
            return function () {
                return inner();
            };
        }
        expect(go({ x: 1, y: 2 })()).toBe(2);
    });

    test("arrow function closure over function declaration", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            return () => inner();
        }
        expect(go({})()).toBe(1);
    });

    test("multiple function declarations", () => {
        function go({ x = 10 }) {
            let y = 20;
            function a() {
                return x;
            }
            function b() {
                return y;
            }
            return function () {
                return a() + b();
            };
        }
        expect(go({})()).toBe(30);
    });

    test("function declaration references another function declaration", () => {
        function go({ x = 5 }) {
            function a() {
                return x;
            }
            function b() {
                return a() * 2;
            }
            return function () {
                return b();
            };
        }
        expect(go({})()).toBe(10);
    });

    test("closure in object literal method shorthand", () => {
        function go({ x = 7 }) {
            function inner() {
                return x;
            }
            return {
                get() {
                    return inner();
                },
            };
        }
        expect(go({}).get()).toBe(7);
    });

    test("closure in computed property value", () => {
        function go({ x = 3 }) {
            function inner() {
                return x;
            }
            let key = "result";
            return {
                [key]: function () {
                    return inner();
                },
            };
        }
        expect(go({}).result()).toBe(3);
    });

    test("closure in getter", () => {
        function go({ x = 11 }) {
            function inner() {
                return x;
            }
            return {
                get value() {
                    return inner();
                },
            };
        }
        expect(go({}).value).toBe(11);
    });

    test("closure in setter", () => {
        function go({ x = 0 }) {
            let captured = x;
            function inner(v) {
                captured = v;
            }
            function get() {
                return captured;
            }
            return {
                set value(v) {
                    inner(v);
                },
                read() {
                    return get();
                },
            };
        }
        let obj = go({});
        obj.value = 42;
        expect(obj.read()).toBe(42);
    });

    test("IIFE closure over function declaration", () => {
        function go({ x = 5 }) {
            function inner() {
                return x;
            }
            return (function () {
                return inner();
            })();
        }
        expect(go({})).toBe(5);
    });

    test("deeply nested closure", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            return function () {
                return function () {
                    return function () {
                        return inner();
                    };
                };
            };
        }
        expect(go({})()()()).toBe(1);
    });

    test("function declaration with same name as destructured binding", () => {
        function go({ x = 1 }) {
            function x() {
                return 42;
            }
            return function () {
                return x();
            };
        }
        expect(go({})()).toBe(42);
    });

    test("var and function declaration coexist", () => {
        function go({ x = 1 }) {
            var z = 100;
            function inner() {
                return x + z;
            }
            return function () {
                return inner();
            };
        }
        expect(go({})()).toBe(101);
    });

    test("let, const, var, and function declaration all present", () => {
        function go({ x = 1 }) {
            let a = 10;
            const b = 20;
            var c = 30;
            function inner() {
                return x + a + b + c;
            }
            return function () {
                return inner();
            };
        }
        expect(go({})()).toBe(61);
    });

    test("function declaration mutates closed-over let", () => {
        function go({ x = 0 }) {
            let counter = x;
            function increment() {
                counter++;
                return counter;
            }
            return function () {
                return increment();
            };
        }
        let inc = go({});
        expect(inc()).toBe(1);
        expect(inc()).toBe(2);
        expect(inc()).toBe(3);
    });

    test("multiple closures share same function declaration", () => {
        function go({ x = 0 }) {
            let state = x;
            function get() {
                return state;
            }
            function set(v) {
                state = v;
            }
            return {
                reader: function () {
                    return get();
                },
                writer: function (v) {
                    set(v);
                },
            };
        }
        let obj = go({});
        expect(obj.reader()).toBe(0);
        obj.writer(42);
        expect(obj.reader()).toBe(42);
    });
});

describe("function declarations accessible from closures with destructured array params", () => {
    test("simple array destructuring", () => {
        function go([x]) {
            function inner() {
                return x;
            }
            return function () {
                return inner();
            };
        }
        expect(go([42])()).toBe(42);
    });

    test("array destructuring with default", () => {
        function go([x = 10]) {
            function inner() {
                return x;
            }
            return function () {
                return inner();
            };
        }
        expect(go([])()).toBe(10);
        expect(go([5])()).toBe(5);
    });

    test("array destructuring with rest", () => {
        function go([first, ...rest]) {
            function inner() {
                return rest.length;
            }
            return function () {
                return inner();
            };
        }
        expect(go([1, 2, 3])()).toBe(2);
    });

    test("nested array destructuring", () => {
        function go([[a, b]]) {
            function inner() {
                return a + b;
            }
            return function () {
                return inner();
            };
        }
        expect(go([[3, 4]])()).toBe(7);
    });

    test("mixed array and object destructuring", () => {
        function go([{ x }], { y }) {
            function inner() {
                return x + y;
            }
            return function () {
                return inner();
            };
        }
        expect(go([{ x: 10 }], { y: 20 })()).toBe(30);
    });
});

describe("function declarations accessible from closures with complex parameter patterns", () => {
    test("plain param before destructured param", () => {
        function go(plain, { x = 1 }) {
            function inner() {
                return plain + x;
            }
            return function () {
                return inner();
            };
        }
        expect(go(100, {})()).toBe(101);
    });

    test("plain param after destructured param", () => {
        function go({ x = 1 }, plain) {
            function inner() {
                return x + plain;
            }
            return function () {
                return inner();
            };
        }
        expect(go({}, 200)()).toBe(201);
    });

    test("rest parameter after destructured param", () => {
        function go({ x = 1 }, ...rest) {
            function inner() {
                return x + rest.length;
            }
            return function () {
                return inner();
            };
        }
        expect(go({}, "a", "b", "c")()).toBe(4);
    });

    test("default parameter expression referencing earlier param", () => {
        function go({ x = 1 }, y = x + 10) {
            function inner() {
                return y;
            }
            return function () {
                return inner();
            };
        }
        expect(go({})()).toBe(11);
    });

    test("parameter default is a function that closes over another param", () => {
        function go({ x = 5 }, fn = () => x) {
            function inner() {
                return fn();
            }
            return function () {
                return inner();
            };
        }
        expect(go({})()).toBe(5);
    });

    test("arguments object interaction", () => {
        function go({ x = 1 }) {
            function inner() {
                return arguments.length;
            }
            return function () {
                return inner();
            };
        }
        // inner() has its own arguments object (length 0)
        expect(go({})()).toBe(0);
    });
});

describe("function declarations in arrow functions with destructured params", () => {
    // Arrow functions don't have their own `arguments` or `this`,
    // but they still create variable environments for function declarations
    // (well, arrow functions can't have function declarations in their body
    // unless using a block, but let's test the block form)

    test("block-body arrow with destructured param", () => {
        let go = ({ x = 1 }) => {
            let y = x + 1;
            // Arrow functions can't have function declarations in non-strict mode
            // but they can have them in blocks... let's use a let-bound function instead
            let inner = function () {
                return y;
            };
            return function () {
                return inner();
            };
        };
        expect(go({})()).toBe(2);
    });
});

describe("method definitions with destructured params", () => {
    test("object method shorthand", () => {
        let obj = {
            go({ x = 5 }) {
                function inner() {
                    return x;
                }
                return function () {
                    return inner();
                };
            },
        };
        expect(obj.go({})()).toBe(5);
    });

    test("class method", () => {
        class C {
            go({ x = 5 }) {
                function inner() {
                    return x;
                }
                return function () {
                    return inner();
                };
            }
        }
        expect(new C().go({})()).toBe(5);
    });

    test("class constructor", () => {
        class C {
            constructor({ x = 5 }) {
                function inner() {
                    return x;
                }
                this.get = function () {
                    return inner();
                };
            }
        }
        expect(new C({}).get()).toBe(5);
    });

    test("static method", () => {
        class C {
            static go({ x = 5 }) {
                function inner() {
                    return x;
                }
                return function () {
                    return inner();
                };
            }
        }
        expect(C.go({})()).toBe(5);
    });
});

describe("generator and async functions with destructured params", () => {
    test("generator function", () => {
        function* go({ x = 5 }) {
            function inner() {
                return x;
            }
            yield function () {
                return inner();
            };
        }
        expect(go({}).next().value()).toBe(5);
    });

    test("async function", () => {
        async function go({ x = 5 }) {
            function inner() {
                return x;
            }
            return function () {
                return inner();
            };
        }
        let resolvedValue;
        go({}).then(fn => {
            resolvedValue = fn();
        });
        runQueuedPromiseJobs();
        expect(resolvedValue).toBe(5);
    });
});

describe("edge cases", () => {
    test("empty destructuring pattern with default triggers separate var env", () => {
        function go({} = {}) {
            function inner() {
                return 42;
            }
            return function () {
                return inner();
            };
        }
        expect(go()()).toBe(42);
    });

    test("deeply nested destructuring with defaults", () => {
        function go({
            a: {
                b: { c = 99 },
            },
        }) {
            function inner() {
                return c;
            }
            return function () {
                return inner();
            };
        }
        expect(go({ a: { b: {} } })()).toBe(99);
    });

    test("destructured param shadows outer variable", () => {
        let x = "outer";
        function go({ x = "default" }) {
            function inner() {
                return x;
            }
            return function () {
                return inner();
            };
        }
        expect(go({})()).toBe("default");
        expect(go({ x: "passed" })()).toBe("passed");
        expect(x).toBe("outer");
    });

    test("function declaration called during object construction in return", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            return {
                a: inner(),
                b: (function () {
                    return inner();
                })(),
                c: (() => inner())(),
            };
        }
        let result = go({ x: 7 });
        expect(result.a).toBe(7);
        expect(result.b).toBe(7);
        expect(result.c).toBe(7);
    });

    test("function declaration used in array literal", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            return [
                inner,
                function () {
                    return inner();
                },
                () => inner(),
            ];
        }
        let result = go({ x: 3 });
        expect(result[0]()).toBe(3);
        expect(result[1]()).toBe(3);
        expect(result[2]()).toBe(3);
    });

    test("function declaration used as callback", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            return [1, 2, 3].map(function () {
                return inner();
            });
        }
        expect(go({ x: 5 })).toEqual([5, 5, 5]);
    });

    test("function declaration in try/catch inside function with destructured params", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            try {
                return function () {
                    return inner();
                };
            } catch (e) {
                return null;
            }
        }
        expect(go({})()).toBe(1);
    });

    test("function declaration in for loop inside function with destructured params", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            let results = [];
            for (let i = 0; i < 3; i++) {
                results.push(function () {
                    return inner() + i;
                });
            }
            return results;
        }
        let fns = go({});
        // let i creates a new binding per iteration
        expect(fns[0]()).toBe(1);
        expect(fns[1]()).toBe(2);
        expect(fns[2]()).toBe(3);
    });

    test("function declaration in switch inside function with destructured params", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            switch (x) {
                case 1:
                    return function () {
                        return inner();
                    };
                default:
                    return function () {
                        return inner() * 2;
                    };
            }
        }
        expect(go({})()).toBe(1);
        expect(go({ x: 5 })()).toBe(10);
    });

    test("function declaration referenced via eval", () => {
        function go({ x = 1 }) {
            function inner() {
                return x;
            }
            return function () {
                return eval("inner()");
            };
        }
        expect(go({ x: 77 })()).toBe(77);
    });

    test("multiple independent closures each see the same function declaration", () => {
        function go({ x = 0 }) {
            let state = x;
            function inc() {
                return ++state;
            }
            let a = function () {
                return inc();
            };
            let b = function () {
                return inc();
            };
            return { a, b };
        }
        let obj = go({});
        expect(obj.a()).toBe(1);
        expect(obj.b()).toBe(2);
        expect(obj.a()).toBe(3);
    });

    test("recursive function declaration inside function with destructured params", () => {
        function go({ n }) {
            function factorial(x) {
                if (x <= 1) return 1;
                return x * factorial(x - 1);
            }
            return function () {
                return factorial(n);
            };
        }
        expect(go({ n: 5 })()).toBe(120);
    });

    test("mutually recursive function declarations", () => {
        function go({ x = true }) {
            function isEven(n) {
                if (n === 0) return true;
                return isOdd(n - 1);
            }
            function isOdd(n) {
                if (n === 0) return false;
                return isEven(n - 1);
            }
            return function (n) {
                return x ? isEven(n) : isOdd(n);
            };
        }
        let checkEven = go({});
        expect(checkEven(4)).toBeTrue();
        expect(checkEven(5)).toBeFalse();
        let checkOdd = go({ x: false });
        expect(checkOdd(3)).toBeTrue();
        expect(checkOdd(4)).toBeFalse();
    });

    test("the original tumblr bug repro", () => {
        function go({ tz: o = 420 }) {
            let s = 69;
            function y() {
                return s;
            }
            return {
                _foo: y,
                _bar: function () {
                    y();
                },
            };
        }
        expect(() => go({})).not.toThrow();
        let result = go({});
        expect(result._foo()).toBe(69);
        expect(() => result._bar()).not.toThrow();
    });
});
