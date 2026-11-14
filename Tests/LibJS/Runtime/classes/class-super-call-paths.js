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
    test("default derived constructors pass the arguments on without the array iterator", () => {
        class A {
            constructor(a, b) {
                this.a = a;
                this.b = b;
                this.count = arguments.length;
            }
        }
        class Default extends A {}
        class DefaultOfDefault extends Default {}
        const original = Array.prototype[Symbol.iterator];
        Array.prototype[Symbol.iterator] = function* () {
            yield "changed";
        };
        try {
            repeat(i => {
                const none = new Default();
                expect(none.count).toBe(0);
                expect(none.a).toBeUndefined();
                const many = new DefaultOfDefault(i, 2, 3);
                expect(many.a).toBe(i);
                expect(many.b).toBe(2);
                expect(many.count).toBe(3);
                expect(many).toBeInstanceOf(DefaultOfDefault);
            });
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

describe("super() with arguments as operands", () => {
    test("this is bound once, also when the parent ran in an inline frame", () => {
        class A {
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            constructor(mode) {
                if (mode === "read before") this.y = 1;
                super(1);
                if (mode === "twice") super(2);
            }
        }
        repeat(i => {
            expect(new B("plain").x).toBe(1);
            if (i % 100 === 0) {
                expect(() => new B("read before")).toThrowWithMessage(
                    ReferenceError,
                    "|this| has not been initialized"
                );
                expect(() => new B("twice")).toThrowWithMessage(ReferenceError, "|this| is already initialized");
            }
        });
    });

    test("return overrides of a derived parent", () => {
        class A {}
        const object = {};
        class P extends A {
            constructor(mode) {
                if (mode !== "no super") super();
                if (mode === "object") return object;
                if (mode === "number") return 1;
            }
        }
        class B extends P {
            constructor(mode) {
                super(mode);
                this.after = true;
            }
        }
        repeat(i => {
            const b = new B("plain");
            expect(b).toBeInstanceOf(B);
            expect(b.after).toBeTrue();
            expect(new B("object")).toBe(object);
            expect(object.after).toBeTrue();
            if (i % 100 === 0) {
                expect(() => new B("number")).toThrowWithMessage(TypeError, "Derived constructor return invalid value");
                expect(() => new B("no super")).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
            }
        });
    });

    test("return overrides of a base parent", () => {
        const object = {};
        class A {
            constructor(mode) {
                this.a = 1;
                if (mode === "object") return object;
                if (mode === "number") return 1;
            }
        }
        class B extends A {
            constructor(mode) {
                super(mode);
                this.b = 2;
            }
        }
        repeat(() => {
            const b = new B("number");
            expect(b).toBeInstanceOf(B);
            expect(b.a).toBe(1);
            expect(new B("object")).toBe(object);
        });
    });

    test("fields and private methods are there right after super() returns", () => {
        class A {
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            field = this.x + 1;
            #secret() {
                return this.field;
            }
            constructor(x) {
                super(x);
                this.seen = this.#secret();
            }
        }
        class C extends B {
            other = this.field * 2;
            constructor(x) {
                super(x);
                this.seen2 = this.other;
            }
        }
        repeat(i => {
            const c = new C(i);
            expect(c.x).toBe(i);
            expect(c.field).toBe(i + 1);
            expect(c.seen).toBe(i + 1);
            expect(c.seen2).toBe(2 * (i + 1));
        });
    });

    test("parents with fields of their own", () => {
        class A {
            a = 1;
            constructor(x) {
                this.x = x + this.a;
            }
        }
        class B extends A {
            constructor(x) {
                super(x);
            }
        }
        repeat(i => expect(new B(i).x).toBe(i + 1));
    });

    test("argument counts above and below the parameter count", () => {
        class A {
            constructor(a, b) {
                this.a = a;
                this.b = b;
                this.count = arguments.length;
            }
        }
        class Fewer extends A {
            constructor() {
                super(1);
            }
        }
        class More extends A {
            constructor() {
                super(1, 2, 3, 4);
            }
        }
        repeat(() => {
            const fewer = new Fewer();
            expect(fewer.b).toBeUndefined();
            expect(fewer.count).toBe(1);
            const more = new More();
            expect(more.b).toBe(2);
            expect(more.count).toBe(4);
        });
    });

    test("three levels with an arrow and eval in the middle", () => {
        class A {
            constructor(x) {
                this.x = x;
                this.target = new.target;
            }
        }
        class B extends A {
            constructor(x) {
                const call = () => super(x);
                call();
            }
        }
        class C extends B {
            constructor(x) {
                super(x);
            }
        }
        class D extends A {
            constructor(x) {
                eval("super(x)");
            }
        }
        class E extends D {
            constructor(x) {
                super(x);
            }
        }
        repeat(i => {
            const c = new C(i);
            expect(c.x).toBe(i);
            expect(c.target).toBe(C);
            expect(new E(i).target).toBe(E);
        });
    });

    test("new.target whose prototype comes from a getter or is not an object", () => {
        class A {
            constructor() {
                this.a = 1;
            }
        }
        class B extends A {
            constructor() {
                super();
            }
        }
        let gets = 0;
        const WithGetter = new Proxy(function () {}, {
            get(target, key) {
                if (key !== "prototype") return target[key];
                gets++;
                return Array.prototype;
            },
        });
        function NotAnObject() {}
        NotAnObject.prototype = 1;
        repeat(() => {
            expect(Object.getPrototypeOf(Reflect.construct(B, [], WithGetter))).toBe(Array.prototype);
            expect(Object.getPrototypeOf(Reflect.construct(B, [], NotAnObject))).toBe(Object.prototype);
        });
        expect(gets).toBe(ITERATIONS);
    });

    test("parent changes while the derived constructor is hot", () => {
        class A {
            constructor() {
                this.from = "A";
            }
        }
        class B extends A {
            constructor(x) {
                super(x);
            }
        }
        const NativeParent = function (x) {
            return new Number(x);
        };
        repeat(i => {
            if (i === ITERATIONS / 2) Object.setPrototypeOf(B, Array);
            const b = new B(3);
            if (i < ITERATIONS / 2) expect(b.from).toBe("A");
            else {
                expect(Array.isArray(b)).toBeTrue();
                expect(b.length).toBe(3);
                expect(b).toBeInstanceOf(B);
            }
        });
        Object.setPrototypeOf(B, NativeParent);
        expect(new B(5).valueOf()).toBe(5);
    });

    test("stack overflow in a recursive construct", () => {
        class A {
            constructor() {}
        }
        class B extends A {
            constructor() {
                super();
                new B();
            }
        }
        expect(() => new B()).toThrow(InternalError);
    });

    test("async and generator methods using super", async () => {
        class A {
            value() {
                return 1;
            }
        }
        class B extends A {
            *generator() {
                yield super.value();
            }
            async asynchronous() {
                await null;
                return super.value() + 1;
            }
        }
        const b = new B();
        repeat(() => expect(b.generator().next().value).toBe(1));
        expect(await b.asynchronous()).toBe(2);
    });
});

describe("prototypes of new objects", () => {
    test("a changed prototype property of the new target is used", () => {
        function Plain() {
            this.a = 1;
        }
        class A {
            constructor() {
                this.a = 1;
            }
        }
        class B extends A {
            constructor() {
                super();
            }
        }
        function Target() {}
        repeat(i => {
            const prototype = { i };
            Plain.prototype = prototype;
            Target.prototype = prototype;
            expect(Object.getPrototypeOf(new Plain())).toBe(prototype);
            expect(Object.getPrototypeOf(Reflect.construct(A, [], Target))).toBe(prototype);
            expect(Object.getPrototypeOf(Reflect.construct(B, [], Target))).toBe(prototype);
            if (i % 100 === 0) {
                Plain.prototype = 1;
                expect(Object.getPrototypeOf(new Plain())).toBe(Object.prototype);
                Target.prototype = null;
                expect(Object.getPrototypeOf(Reflect.construct(B, [], Target))).toBe(Object.prototype);
            }
        });
    });
});
