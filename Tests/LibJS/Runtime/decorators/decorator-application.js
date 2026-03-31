// Decorator application tests -- decorators that actually transform behavior.

describe("method decorators", () => {
    test("method decorator can replace the method", () => {
        function doubleReturn(method) {
            return function (...args) {
                return method.call(this, ...args) * 2;
            };
        }

        class A {
            @doubleReturn
            value() {
                return 21;
            }
        }

        expect(new A().value()).toBe(42);
    });

    test("method decorator returning undefined keeps original", () => {
        const log = [];
        function trace(method, context) {
            log.push(context.name);
        }

        class A {
            @trace
            foo() {
                return 1;
            }
        }

        expect(new A().foo()).toBe(1);
        expect(log).toEqual(["foo"]);
    });

    test("multiple method decorators applied inner-to-outer", () => {
        const log = [];
        function dec(id) {
            return function (method, context) {
                log.push(`apply ${id}`);
                return function (...args) {
                    log.push(`call ${id}`);
                    return method.call(this, ...args);
                };
            };
        }

        class A {
            @dec("outer")
            @dec("inner")
            foo() {
                return 1;
            }
        }

        expect(log).toEqual(["apply inner", "apply outer"]);
        log.length = 0;

        new A().foo();
        expect(log).toEqual(["call outer", "call inner"]);
    });

    test("static method decorator", () => {
        function doubleReturn(method) {
            return function (...args) {
                return method.call(this, ...args) * 2;
            };
        }

        class A {
            @doubleReturn
            static value() {
                return 21;
            }
        }

        expect(A.value()).toBe(42);
    });

    test("private method decorator", () => {
        function doubleReturn(method) {
            return function (...args) {
                return method.call(this, ...args) * 2;
            };
        }

        class A {
            @doubleReturn
            #value() {
                return 21;
            }

            getValue() {
                return this.#value();
            }
        }

        expect(new A().getValue()).toBe(42);
    });
});

describe("field decorators", () => {
    test("field decorator can add initializer", () => {
        function double(value, context) {
            return function (initialValue) {
                return initialValue * 2;
            };
        }

        class A {
            @double
            x = 21;
        }

        expect(new A().x).toBe(42);
    });

    test("field decorator returning undefined keeps original", () => {
        function noop() {}

        class A {
            @noop
            x = 42;
        }

        expect(new A().x).toBe(42);
    });

    test("multiple field decorators chain initializers", () => {
        function times(n) {
            return function () {
                return function (v) {
                    return v * n;
                };
            };
        }

        class A {
            @times(2)
            @times(3)
            x = 1;
        }

        // Inner (times(3)) runs first on initial value: 1 * 3 = 3
        // Outer (times(2)) runs on that result: 3 * 2 = 6
        expect(new A().x).toBe(6);
    });
});

describe("getter decorators", () => {
    test("getter decorator can replace the getter", () => {
        function cache(getter) {
            return function () {
                if (!this._cached) {
                    this._cached = getter.call(this);
                }
                return this._cached;
            };
        }

        class A {
            @cache
            get value() {
                return Math.random();
            }
        }

        const a = new A();
        const v1 = a.value;
        const v2 = a.value;
        expect(v1).toBe(v2);
    });
});

describe("setter decorators", () => {
    test("setter decorator can replace the setter", () => {
        function validate(setter) {
            return function (value) {
                if (typeof value !== "number") throw new TypeError("expected number");
                setter.call(this, value);
            };
        }

        class A {
            _x = 0;

            @validate
            set x(v) {
                this._x = v;
            }
        }

        const a = new A();
        a.x = 42;
        expect(a._x).toBe(42);
        expect(() => {
            a.x = "not a number";
        }).toThrowWithMessage(TypeError, "expected number");
    });
});

describe("auto-accessor decorators", () => {
    test("accessor decorator can replace get and set", () => {
        function logged(value, context) {
            const { get, set } = value;
            return {
                get() {
                    return get.call(this);
                },
                set(v) {
                    set.call(this, v);
                },
            };
        }

        class A {
            @logged
            accessor x = 1;
        }

        const a = new A();
        expect(a.x).toBe(1);
        a.x = 2;
        expect(a.x).toBe(2);
    });

    test("accessor decorator can provide init", () => {
        function withDefault(value, context) {
            return {
                init(v) {
                    return v ?? 42;
                },
            };
        }

        class A {
            @withDefault
            accessor x;
        }

        expect(new A().x).toBe(42);
    });
});

describe("class decorators", () => {
    test("class decorator can replace the class", () => {
        function addMethod(cls, context) {
            return class extends cls {
                added() {
                    return 42;
                }
            };
        }

        @addMethod
        class A {}

        expect(new A().added()).toBe(42);
    });

    test("class decorator returning undefined keeps original", () => {
        const log = [];
        function track(cls, context) {
            log.push(context.name);
        }

        @track
        class MyClass {}

        expect(log).toEqual(["MyClass"]);
        expect(new MyClass()).toBeInstanceOf(MyClass);
    });

    test("multiple class decorators applied inner-to-outer", () => {
        const log = [];
        function dec(id) {
            return function (cls, context) {
                log.push(id);
            };
        }

        @dec("outer")
        @dec("inner")
        class A {}

        expect(log).toEqual(["inner", "outer"]);
    });
});

describe("decorator errors", () => {
    test("method decorator returning non-function throws", () => {
        function bad() {
            return 42;
        }

        expect(() => {
            class A {
                @bad
                method() {}
            }
        }).toThrowWithMessage(TypeError, "Decorator must return a function or undefined");
    });

    test("class decorator returning non-function throws", () => {
        function bad() {
            return 42;
        }

        expect(() => {
            @bad
            class A {}
        }).toThrowWithMessage(TypeError, "Decorator must return a function or undefined");
    });
});
