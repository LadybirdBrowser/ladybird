// NB: Each case runs many times so that the fast paths of the interpreter and the JIT take over from the slow ones.
const ITERATIONS = 2000;

function repeat(callback) {
    for (let i = 0; i < ITERATIONS; i++) callback(i);
}

describe("construct results of derived constructors", () => {
    test("this, object overrides and errors", () => {
        class A {}
        const object = {};
        class B extends A {
            constructor(mode) {
                if (mode !== "no super") super();
                if (mode === "object") return object;
                if (mode === "number") return 1;
                if (mode === "undefined") return undefined;
            }
        }
        repeat(i => {
            expect(new B("plain")).toBeInstanceOf(B);
            expect(new B("undefined")).toBeInstanceOf(B);
            expect(new B("object")).toBe(object);
            if (i % 100 === 0) {
                expect(() => new B("number")).toThrow(TypeError);
                expect(() => new B("no super")).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
            }
        });
    });

    test("a primitive return without super() is a TypeError", () => {
        class A {}
        class B extends A {
            constructor() {
                return 1;
            }
        }
        repeat(() => expect(() => new B()).toThrow(TypeError));
    });
});

describe("arguments of super()", () => {
    test("spread and forwarded arguments", () => {
        class A {
            constructor(...args) {
                this.args = args;
            }
        }
        class Spread extends A {
            constructor(a, ...rest) {
                super(...rest, a);
            }
        }
        class Default extends A {}
        repeat(i => {
            expect(new Spread(i, 1, 2).args).toEqual([1, 2, i]);
            expect(new Default(i, undefined).args).toEqual([i, undefined]);
            expect(new Default().args).toEqual([]);
        });
    });

    test("an array iterator that is changed is still used by the spread", () => {
        class A {
            constructor(...args) {
                this.args = args;
            }
        }
        class B extends A {
            constructor(...args) {
                super(...args);
            }
        }
        const original = Array.prototype[Symbol.iterator];
        repeat(i => expect(new B(i).args).toEqual([i]));
        Array.prototype[Symbol.iterator] = function* () {
            yield "changed";
        };
        try {
            expect(new B(1).args).toEqual(["changed"]);
        } finally {
            Array.prototype[Symbol.iterator] = original;
        }
    });
});

describe("parents of derived constructors", () => {
    test("native constructors, Proxies and bound functions", () => {
        class FromArray extends Array {}
        class FromError extends Error {}
        let constructs = 0;
        const Proxied = new Proxy(
            class {
                constructor(x) {
                    this.x = x;
                }
            },
            {
                construct(target, args, newTarget) {
                    constructs++;
                    return Reflect.construct(target, args, newTarget);
                },
            }
        );
        class FromProxy extends Proxied {}
        function Plain(x) {
            this.x = x;
        }
        const Bound = Plain.bind(null, 42);
        Bound.prototype = Plain.prototype;
        class FromBound extends Bound {}
        repeat(i => {
            const array = new FromArray(3);
            expect(array.length).toBe(3);
            expect(array).toBeInstanceOf(FromArray);
            expect(new FromError("m").message).toBe("m");
            expect(new FromProxy(i).x).toBe(i);
            expect(new FromBound().x).toBe(42);
        });
        expect(constructs).toBe(ITERATIONS);
    });

    test("parent changed with Object.setPrototypeOf", () => {
        class A {
            constructor() {
                this.from = "A";
            }
        }
        class C {
            constructor() {
                this.from = "C";
            }
        }
        class B extends A {
            constructor() {
                super();
            }
        }
        class D extends A {}
        repeat(() => {
            expect(new B().from).toBe("A");
            expect(new D().from).toBe("A");
        });
        Object.setPrototypeOf(B, C);
        Object.setPrototypeOf(D, C);
        expect(new B().from).toBe("C");
        expect(new D().from).toBe("C");
        Object.setPrototypeOf(B, null);
        expect(() => new B()).toThrow(TypeError);
    });

    test("exceptions from the parent constructor", () => {
        class A {
            constructor(fail) {
                if (fail) throw new Error("parent");
                this.ok = true;
            }
        }
        class B extends A {
            constructor(fail) {
                try {
                    super(fail);
                } catch (e) {
                    return { caught: e.message };
                }
            }
        }
        class C extends A {}
        repeat(i => {
            const fail = i % 10 === 0;
            const result = new B(fail);
            if (fail) expect(result.caught).toBe("parent");
            else expect(result.ok).toBeTrue();
            if (fail) expect(() => new C(true)).toThrowWithMessage(Error, "parent");
        });
    });

    test("Reflect.construct with another new.target", () => {
        class A {
            constructor() {
                this.target = new.target;
            }
        }
        class B extends A {
            constructor() {
                super();
                this.ownTarget = new.target;
            }
        }
        class Default extends A {}
        function Other() {}
        repeat(() => {
            const b = Reflect.construct(B, [], Other);
            expect(b.target).toBe(Other);
            expect(b.ownTarget).toBe(Other);
            expect(Object.getPrototypeOf(b)).toBe(Other.prototype);
            expect(Reflect.construct(Default, [], Other).target).toBe(Other);
            expect(new B().target).toBe(B);
        });
    });
});

describe("super property access", () => {
    test("getters on the prototype chain changing between calls", () => {
        class A {
            get value() {
                return "A";
            }
        }
        class B extends A {
            read() {
                return super.value;
            }
        }
        const b = new B();
        repeat(() => expect(b.read()).toBe("A"));
        Object.defineProperty(A.prototype, "value", { get: () => "changed", configurable: true });
        expect(b.read()).toBe("changed");
        Object.setPrototypeOf(B.prototype, {
            get value() {
                return "new parent";
            },
        });
        expect(b.read()).toBe("new parent");
    });

    test("methods called from a hot caller", () => {
        class A {
            method(x) {
                return x + this.offset;
            }
        }
        class B extends A {
            constructor() {
                super();
                this.offset = 1;
            }
            method(x) {
                return super.method(x) * 2;
            }
        }
        const b = new B();
        let sum = 0;
        repeat(i => (sum += b.method(i)));
        expect(sum).toBe(ITERATIONS * (ITERATIONS + 1));
        const otherHome = {
            method(x) {
                return super.method(x);
            },
        };
        Object.setPrototypeOf(otherHome, { method: x => x * 3 });
        let other = 0;
        repeat(i => (other += otherHome.method(1)));
        expect(other).toBe(3 * ITERATIONS);
    });
});
