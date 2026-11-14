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

describe("super property loads and new.target in hot code", () => {
    test("a super method that becomes a getter or changes", () => {
        class A {
            value() {
                return 1;
            }
        }
        class B extends A {
            read() {
                return super.value;
            }
            call() {
                return super.value();
            }
        }
        const b = new B();
        repeat(i => {
            if (i === ITERATIONS / 2) A.prototype.value = () => 2;
            expect(b.call()).toBe(i < ITERATIONS / 2 ? 1 : 2);
        });
        Object.defineProperty(A.prototype, "value", {
            get() {
                return this;
            },
            configurable: true,
        });
        expect(b.read()).toBe(b);
        Object.setPrototypeOf(B.prototype, { value: "other" });
        expect(b.read()).toBe("other");
    });

    test("new.target of called and constructed functions", () => {
        function target() {
            return new.target;
        }
        function Constructed() {
            this.target = new.target;
        }
        class Base {
            constructor() {
                this.target = new.target;
            }
        }
        function outer() {
            return (() => new.target)();
        }
        repeat(() => {
            expect(target()).toBeUndefined();
            expect(new Constructed().target).toBe(Constructed);
            expect(new Base().target).toBe(Base);
            expect(outer()).toBeUndefined();
            expect(new outer()).toBe(outer);
        });
    });
});

describe("derived constructs in hot callers", () => {
    test("this, new.target and prototypes", () => {
        class A {
            constructor(x) {
                this.x = x;
                this.parentTarget = new.target;
            }
        }
        class B extends A {
            constructor(x, y) {
                super(x);
                this.y = y;
                this.ownTarget = new.target;
            }
        }
        function make(i) {
            return new B(i, i + 1);
        }
        repeat(i => {
            const b = make(i);
            expect(b.x).toBe(i);
            expect(b.y).toBe(i + 1);
            expect(b.parentTarget).toBe(B);
            expect(b.ownTarget).toBe(B);
            expect(Object.getPrototypeOf(b)).toBe(B.prototype);
            expect(Object.keys(b)).toEqual(["x", "parentTarget", "y", "ownTarget"]);
        });
    });

    test("argument counts above and below the parameter counts", () => {
        class A {
            constructor(a, b, c) {
                this.values = [a, b, c, arguments.length];
            }
        }
        class B extends A {
            constructor(a, b) {
                super(a);
                this.b = b;
            }
        }
        class C extends A {
            constructor(a) {
                super(a, a, a, a);
            }
        }
        repeat(i => {
            expect(new B().values).toEqual([undefined, undefined, undefined, 1]);
            expect(new B(i, 2, 3).values).toEqual([i, undefined, undefined, 1]);
            expect(new B(i, 2, 3).b).toBe(2);
            expect(new C(i).values).toEqual([i, i, i, 4]);
        });
    });

    test("values of other types before, in and after super()", () => {
        class A {
            constructor(x) {
                this.x = x.value + 1;
            }
        }
        class B extends A {
            constructor(x, y) {
                super(x.inner);
                this.y = y.value * 2;
            }
        }
        function make(i, kind) {
            const inner = kind === 1 ? { value: "s" } : { value: i };
            const x = kind === 0 ? { inner } : { other: 1, inner };
            const y = kind === 2 ? { value: "2" } : { value: i };
            return new B(x, y);
        }
        for (let kind = 0; kind < 3; kind++) {
            repeat(i => {
                const b = make(i, i < ITERATIONS / 2 ? 0 : kind);
                const changed = i >= ITERATIONS / 2 ? kind : 0;
                expect(b.x).toBe(changed === 1 ? "s1" : i + 1);
                expect(b.y).toBe(changed === 2 ? 4 : i * 2);
                expect(b).toBeInstanceOf(B);
            });
        }
    });

    test("exceptions from the parent and after super()", () => {
        class A {
            constructor(fail) {
                if (fail === 1) throw new Error("parent");
                this.ok = true;
            }
        }
        class B extends A {
            constructor(fail) {
                super(fail);
                if (fail === 2) throw new Error("derived");
                this.done = true;
            }
        }
        function make(fail) {
            return new B(fail);
        }
        repeat(i => {
            const fail = i % 50 === 0 ? 1 + ((i / 50) % 2) : 0;
            if (fail === 0) {
                const b = make(0);
                expect(b.ok).toBeTrue();
                expect(b.done).toBeTrue();
            } else {
                let caught;
                try {
                    make(fail);
                } catch (e) {
                    caught = e;
                }
                expect(caught.message).toBe(fail === 1 ? "parent" : "derived");
                const frames = caught.stack.split("\n").map(line => line.trim().split(" ")[1]);
                expect(frames.slice(1, fail === 1 ? 4 : 3)).toEqual(fail === 1 ? ["A", "B", "make"] : ["B", "make"]);
            }
        });
    });

    test("a parent changed with Object.setPrototypeOf while the caller is hot", () => {
        class A {
            constructor(x) {
                this.from = "A";
                this.x = x;
            }
        }
        class C {
            constructor(x) {
                this.from = "C";
                this.x = x;
            }
        }
        class B extends A {
            constructor(x) {
                super(x);
                this.y = 1;
            }
        }
        function make(i) {
            return new B(i);
        }
        repeat(i => {
            if (i === ITERATIONS / 2) Object.setPrototypeOf(B, C);
            const b = make(i);
            expect(b.from).toBe(i < ITERATIONS / 2 ? "A" : "C");
            expect(b.x).toBe(i);
            expect(b.y).toBe(1);
            expect(b).toBeInstanceOf(B);
        });
    });

    test("Reflect.construct with another new.target while new is hot", () => {
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
        function Other() {}
        function make() {
            return new B();
        }
        repeat(i => {
            expect(make().target).toBe(B);
            if (i % 100 === 0) {
                const other = Reflect.construct(B, [], Other);
                expect(other.target).toBe(Other);
                expect(other.ownTarget).toBe(Other);
                expect(Object.getPrototypeOf(other)).toBe(Other.prototype);
            }
        });
    });

    test("three levels of inheritance", () => {
        class A {
            constructor(x) {
                this.a = x;
            }
        }
        class B extends A {
            constructor(x) {
                super(x);
                this.b = x + 1;
            }
        }
        class C extends B {
            constructor(x) {
                super(x);
                this.c = x + 2;
            }
        }
        function make(i) {
            return new C(i);
        }
        repeat(i => {
            const c = make(i);
            expect([c.a, c.b, c.c]).toEqual([i, i + 1, i + 2]);
            expect(c).toBeInstanceOf(C);
            expect(new B(i).b).toBe(i + 1);
        });
    });

    test("default derived constructors", () => {
        class A {
            constructor(a, b) {
                this.a = a;
                this.b = b;
                this.target = new.target;
            }
        }
        class B extends A {}
        function make(i) {
            if (i % 3 === 0) return new B();
            if (i % 3 === 1) return new B(i);
            return new B(i, i + 1, i + 2);
        }
        repeat(i => {
            const b = make(i);
            const count = i % 3 === 0 ? 0 : i % 3 === 1 ? 1 : 3;
            expect(b.a).toBe(count > 0 ? i : undefined);
            expect(b.b).toBe(count > 1 ? i + 1 : undefined);
            expect(b.target).toBe(B);
            expect(b).toBeInstanceOf(B);
        });
    });

    test("default derived constructors whose parent changes while the caller is hot", () => {
        class A {
            constructor(x) {
                this.from = "A";
                this.x = x;
            }
        }
        class C {
            constructor(x) {
                this.from = "C";
                this.x = x;
            }
        }
        class B extends A {}
        function make(i) {
            return new B(i);
        }
        repeat(i => {
            if (i === ITERATIONS / 2) Object.setPrototypeOf(B, C);
            const b = make(i);
            expect(b.from).toBe(i < ITERATIONS / 2 ? "A" : "C");
            expect(b.x).toBe(i);
        });
    });

    test("classes with fields and private methods", () => {
        class A {
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            field = 1;
            #secret() {
                return this.x;
            }
            constructor(x) {
                super(x);
                this.y = this.#secret() + this.field;
            }
        }
        function make(i) {
            return new B(i);
        }
        repeat(i => {
            const b = make(i);
            expect(b.y).toBe(i + 1);
            expect(Object.keys(b)).toEqual(["x", "field", "y"]);
        });
    });

    test("a handler that catches what super() throws", () => {
        class A {
            constructor(fail) {
                if (fail) throw new Error("failed");
                this.x = 1;
            }
        }
        class B extends A {
            constructor(fail) {
                try {
                    super(fail);
                } catch {}
            }
        }
        function make(fail) {
            return new B(fail);
        }
        repeat(() => {
            expect(make(false).x).toBe(1);
        });
        // NB: The handler runs for the first time here, and the constructor returns without a this value.
        expect(() => make(true)).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
        repeat(i => {
            if (i % 100 === 0) {
                expect(() => make(true)).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
            } else {
                expect(make(false).x).toBe(1);
            }
        });
    });
});

describe("inheritance chains in hot callers", () => {
    test("each level sees the new target and runs in order", () => {
        const log = [];
        class A {
            constructor(x) {
                this.x = x;
                this.aTarget = new.target;
            }
        }
        class B extends A {
            constructor(x) {
                super(x);
                this.bTarget = new.target;
            }
        }
        class C extends B {}
        class D extends C {
            constructor(x) {
                super(x + 1);
                this.d = this.x;
            }
        }
        function make(i) {
            return new D(i);
        }
        repeat(i => {
            const d = make(i);
            expect(d.x).toBe(i + 1);
            expect(d.d).toBe(i + 1);
            expect(d.aTarget).toBe(D);
            expect(d.bTarget).toBe(D);
            expect(Object.getPrototypeOf(d)).toBe(D.prototype);
            expect(Object.keys(d)).toEqual(["x", "aTarget", "bTarget", "d"]);
        });
    });

    test("return overrides and exceptions in the middle of a chain", () => {
        const override = { override: true };
        class A {
            constructor(mode) {
                if (mode === "throw") throw new Error("base");
                this.a = 1;
            }
        }
        class B extends A {
            constructor(mode) {
                super(mode);
                if (mode === "override") return override;
                if (mode === "number") return 1;
            }
        }
        class C extends B {
            constructor(mode) {
                super(mode);
                this.c = 1;
            }
        }
        function make(mode) {
            return new C(mode);
        }
        repeat(i => {
            expect(make("plain").c).toBe(1);
            if (i % 100 === 0) {
                const result = make("override");
                expect(result).toBe(override);
                expect(result.c).toBe(1);
                delete override.c;
                expect(() => make("number")).toThrow(TypeError);
                expect(() => make("throw")).toThrowWithMessage(Error, "base");
            }
        });
    });

    test("changing a prototype in the chain while hot", () => {
        class A {
            constructor() {
                this.from = "A";
            }
        }
        class Other {
            constructor() {
                this.from = "Other";
            }
        }
        class B extends A {
            constructor() {
                super();
            }
        }
        class C extends B {
            constructor() {
                super();
            }
        }
        function make() {
            return new C();
        }
        repeat(i => {
            if (i === ITERATIONS / 2) Object.setPrototypeOf(B, Other);
            expect(make().from).toBe(i < ITERATIONS / 2 ? "A" : "Other");
        });
    });

    test("Reflect.construct with another new target", () => {
        class A {
            constructor() {
                this.target = new.target;
            }
        }
        class B extends A {}
        class C extends B {}
        function make(i) {
            return i % 3 === 0 ? Reflect.construct(C, [], Array) : new C();
        }
        repeat(i => {
            const result = make(i);
            expect(result.target).toBe(i % 3 === 0 ? Array : C);
            expect(Object.getPrototypeOf(result)).toBe(i % 3 === 0 ? Array.prototype : C.prototype);
        });
    });

    test("a chain of classes from one class expression", () => {
        const mixin = Base =>
            class extends Base {
                constructor(x) {
                    super(x);
                    this.depth = (this.depth ?? 0) + 1;
                }
            };
        class A {
            constructor(x) {
                this.x = x;
            }
        }
        const B = mixin(mixin(mixin(mixin(A))));
        function make(i) {
            return new B(i);
        }
        repeat(i => {
            const b = make(i);
            expect(b.x).toBe(i);
            expect(b.depth).toBe(4);
        });
    });
});

describe("instance elements of derived constructs in hot callers", () => {
    test("fields of each level in order, right after each super() returns", () => {
        const log = [];
        class A {
            a = (log.push("a"), 1);
            constructor() {
                log.push("A body");
            }
        }
        class B extends A {
            b = (log.push("b"), this.a + 1);
            constructor() {
                log.push("B before");
                super();
                log.push("B after");
            }
        }
        class C extends B {
            c = (log.push("c"), this.b + 1);
            constructor() {
                super();
                log.push("C after");
            }
        }
        function make() {
            return new C();
        }
        repeat(() => {
            log.length = 0;
            const c = make();
            expect(log).toEqual(["B before", "a", "A body", "b", "B after", "c", "C after"]);
            expect(Object.keys(c)).toEqual(["a", "b", "c"]);
            expect(c.c).toBe(3);
        });
    });

    test("initializers see this, earlier fields, private names and no new.target", () => {
        class A {
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            #secret = this.x * 2;
            first = this.#secret + 1;
            second = this.first + 1;
            target = new.target;
            arrow = () => this.x;
            constructor(x) {
                super(x);
            }
            secret() {
                return this.#secret;
            }
        }
        function make(i) {
            return new B(i);
        }
        repeat(i => {
            const b = make(i);
            expect(b.secret()).toBe(i * 2);
            expect(b.first).toBe(i * 2 + 1);
            expect(b.second).toBe(i * 2 + 2);
            expect(b.target).toBeUndefined();
            expect(b.arrow()).toBe(i);
        });
    });

    test("private methods and accessors are installed before fields", () => {
        class A {}
        class B extends A {
            seen = this.#method() + this.#accessor;
            #method() {
                return 1;
            }
            get #accessor() {
                return 2;
            }
            constructor() {
                super();
            }
        }
        function make() {
            return new B();
        }
        repeat(() => expect(make().seen).toBe(3));
    });

    test("fields are defined, not set, and initializers may throw", () => {
        let setterCalls = 0;
        class A {
            set field(value) {
                setterCalls++;
            }
        }
        class B extends A {
            field = 1;
            other = B.fail
                ? (() => {
                      throw new Error("initializer");
                  })()
                : 2;
            constructor() {
                super();
            }
        }
        function make() {
            return new B();
        }
        repeat(i => {
            B.fail = i % 100 === 99;
            if (B.fail) {
                expect(make).toThrowWithMessage(Error, "initializer");
                return;
            }
            const b = make();
            expect(Object.getOwnPropertyDescriptor(b, "field").value).toBe(1);
            expect(b.other).toBe(2);
        });
        expect(setterCalls).toBe(0);
    });

    test("fields on objects returned by a parent", () => {
        let returned = null;
        class A {
            constructor() {
                if (returned) return returned;
            }
        }
        class B extends A {
            field = 1;
            #private = 2;
            constructor() {
                super();
            }
            static hasPrivate(object) {
                return #private in object;
            }
        }
        function make() {
            return new B();
        }
        repeat(i => {
            returned = null;
            if (i % 100 === 50) {
                returned = Object.freeze({});
                expect(make).toThrow(TypeError);
                returned = {};
                const object = make();
                expect(object).toBe(returned);
                expect(B.hasPrivate(object)).toBeTrue();
                expect(make).toThrow(TypeError);
                return;
            }
            expect(make().field).toBe(1);
        });
    });

    test("computed field names are evaluated once", () => {
        let evaluations = 0;
        class A {}
        class B extends A {
            [(evaluations++, "computed")] = 1;
            constructor() {
                super();
            }
        }
        function make() {
            return new B();
        }
        repeat(() => expect(make().computed).toBe(1));
        expect(evaluations).toBe(1);
    });
});

describe("fields of base constructs in hot callers", () => {
    test("literal fields, duplicate and symbol names, and the body after them", () => {
        const symbol = Symbol("field");
        class A {
            a = 1;
            b;
            a = 3;
            [symbol] = true;
            "quoted name" = null;
            constructor(x) {
                this.seenA = this.a;
                this.x = x;
            }
        }
        function make(i) {
            return new A(i);
        }
        repeat(i => {
            const a = make(i);
            expect(Object.keys(a)).toEqual(["a", "b", "quoted name", "seenA", "x"]);
            expect(a.a).toBe(3);
            expect(a.b).toBeUndefined();
            expect(a[symbol]).toBeTrue();
            expect(a.seenA).toBe(3);
            expect(a.x).toBe(i);
            expect(Object.getOwnPropertyDescriptor(a, "a")).toEqual({
                value: 3,
                writable: true,
                enumerable: true,
                configurable: true,
            });
        });
    });

    test("fields are defined, not set, on objects with setters in the prototype chain", () => {
        let setterCalls = 0;
        class A {
            field = 1;
            set field(value) {
                setterCalls++;
            }
        }
        function make() {
            return new A();
        }
        repeat(() => expect(Object.getOwnPropertyDescriptor(make(), "field").value).toBe(1));
        expect(setterCalls).toBe(0);
    });

    test("parents with fields under derived constructors, new targets and prototypes changing while hot", () => {
        class A {
            fromA = 1;
            constructor() {
                this.target = new.target;
            }
        }
        class B extends A {
            constructor() {
                super();
                this.fromB = this.fromA + 1;
            }
        }
        class C extends B {
            fromC = 3;
        }
        class Other {
            fromOther = 4;
        }
        function make(i) {
            return i % 7 === 0 ? Reflect.construct(B, [], Array) : new C();
        }
        repeat(i => {
            if (i === ITERATIONS / 2) Object.setPrototypeOf(B, Other);
            const result = make(i);
            if (i % 7 === 0) {
                expect(result.target).toBe(i < ITERATIONS / 2 ? Array : undefined);
                expect(Object.getPrototypeOf(result)).toBe(Array.prototype);
                return;
            }
            expect(Object.getPrototypeOf(result)).toBe(C.prototype);
            expect(result.fromC).toBe(3);
            if (i < ITERATIONS / 2) {
                expect(Object.keys(result)).toEqual(["fromA", "target", "fromB", "fromC"]);
            } else {
                expect(Object.keys(result)).toEqual(["fromOther", "fromB", "fromC"]);
                expect(result.fromB).toBeNaN();
            }
        });
    });

    test("the prototype of the prototype of the new target changing while hot", () => {
        class A {
            field = 1;
        }
        class B extends A {}
        const proto = { custom: true };
        function make() {
            return new B();
        }
        repeat(i => {
            if (i === ITERATIONS / 2) Object.setPrototypeOf(B.prototype, proto);
            const b = make();
            expect(b.field).toBe(1);
            expect(Object.getPrototypeOf(b)).toBe(B.prototype);
            expect(b.custom).toBe(i < ITERATIONS / 2 ? undefined : true);
        });
    });
});

describe("constructs in hot callers that are not inlined", () => {
    // NB: A branch before super() and sites that see several constructors keep constructs from being inlined.
    test("return overrides of base and derived constructors", () => {
        const object = {};
        class A {
            constructor(mode) {
                if (mode === "object") return object;
                if (mode === "number") return 1;
            }
        }
        class B extends A {
            constructor(mode) {
                if (mode === "no super") {
                    if (mode === "never") super();
                } else if (mode === "twice") {
                    super();
                    super();
                } else {
                    super(mode === "parent object" ? "object" : undefined);
                }
                if (mode === "object") return object;
                if (mode === "number") return 1;
                if (mode === "undefined") return undefined;
                if (mode === "this before super") return this;
            }
        }
        const constructors = [A, B];
        repeat(i => {
            const C = constructors[i & 1];
            expect(new C("plain")).toBeInstanceOf(C);
            expect(new C("object")).toBe(object);
            if (C === A) {
                expect(new C("number")).toBeInstanceOf(A);
                return;
            }
            expect(new C("undefined")).toBeInstanceOf(B);
            expect(new C("parent object")).toBe(object);
            if (i % 50 === 1) {
                expect(() => new C("number")).toThrowWithMessage(TypeError, "Derived constructor return invalid value");
                expect(() => new C("no super")).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
                expect(() => new C("twice")).toThrowWithMessage(ReferenceError, "|this| is already initialized");
            }
        });
    });

    test("exceptions at each stage keep the constructor in stack traces", () => {
        class A {
            constructor(stage) {
                if (stage === "base") throw new Error("base");
            }
        }
        class B extends A {
            constructor(stage) {
                if (stage === "before super") throw new Error("before super");
                if (stage) super(stage);
                else super(stage);
                if (stage === "after super") throw new Error("after super");
            }
        }
        repeat(i => {
            expect(new B(undefined)).toBeInstanceOf(B);
            if (i % 50 !== 0) return;
            for (const stage of ["base", "before super", "after super"]) {
                let caught;
                try {
                    new B(stage);
                } catch (error) {
                    caught = error;
                }
                expect(caught.message).toBe(stage);
                expect(caught.stack.includes("at B (")).toBeTrue();
                if (stage === "base") expect(caught.stack.includes("at A (")).toBeTrue();
            }
        });
    });

    test("new.target in the constructor and through super()", () => {
        let seen = [];
        class A {
            constructor() {
                seen.push(new.target);
            }
        }
        class B extends A {
            constructor(flag) {
                seen.push(new.target);
                if (flag) super();
                else super();
            }
        }
        class C extends B {}
        const constructors = [A, B, C];
        repeat(i => {
            seen = [];
            const D = constructors[i % 3];
            new D(i & 1);
            expect(seen.every(target => target === D)).toBeTrue();
            seen = [];
            const object = Reflect.construct(B, [i & 1], C);
            expect(Object.getPrototypeOf(object)).toBe(C.prototype);
            expect(seen).toEqual([C, C]);
        });
    });

    test("prototypes that change, are getters or are not objects", () => {
        function F() {
            this.a = 1;
        }
        class B extends F {
            constructor(flag) {
                if (flag) super();
                else super();
            }
        }
        const original = F.prototype;
        const replacement = {};
        const getterTarget = new Proxy(function () {}, {
            get: (target, key) => (key === "prototype" ? replacement : target[key]),
        });
        repeat(i => {
            const C = i & 1 ? F : B;
            const mode = i % 5;
            if (mode === 0) F.prototype = original;
            if (mode === 1) F.prototype = replacement;
            if (mode === 2) F.prototype = 1;
            if (mode === 3) {
                F.prototype = original;
                const object = Reflect.construct(C, [i & 2], getterTarget);
                expect(Object.getPrototypeOf(object)).toBe(replacement);
                expect(object.a).toBe(1);
                return;
            }
            if (mode === 4) F.prototype = null;
            const object = new C(i & 2);
            const expected = C === B ? B.prototype : F.prototype;
            if (typeof expected === "object" && expected !== null) expect(Object.getPrototypeOf(object)).toBe(expected);
            else expect(Object.getPrototypeOf(object)).toBe(Object.prototype);
            expect(object.a).toBe(1);
        });
        F.prototype = original;
    });

    test("a throwing prototype getter of the new target", () => {
        class A {
            constructor() {
                this.a = 1;
            }
        }
        let throws = false;
        const Target = new Proxy(function () {}, {
            get(target, key) {
                if (key !== "prototype") return target[key];
                if (throws) throw new Error("prototype");
                return A.prototype;
            },
        });
        const targets = [A, Target];
        repeat(i => {
            throws = i % 10 === 0;
            if (throws) expect(() => Reflect.construct(A, [], Target)).toThrowWithMessage(Error, "prototype");
            else expect(Reflect.construct(A, [], targets[i & 1]).a).toBe(1);
        });
    });

    test("a polymorphic site with base, derived, plain, bound and native constructors", () => {
        class A {
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            constructor(x) {
                super(x + 1);
            }
        }
        function F(x) {
            this.x = x + 2;
        }
        const Bound = A.bind(null, 10);
        const constructors = [A, B, F, Bound, Array, Object, Date];
        repeat(i => {
            const C = constructors[i % constructors.length];
            const object = new C(i);
            if (C === Array) expect(object.length).toBe(i);
            else if (C === Object) expect(object instanceof Number).toBeTrue();
            else if (C === Date) expect(object).toBeInstanceOf(Date);
            else if (C === Bound) expect(object.x).toBe(10);
            else expect(object.x).toBe(i + (C === B ? 1 : C === F ? 2 : 0));
        });
    });

    test("class constructors called without new throw", () => {
        class A {}
        class B extends A {}
        const constructors = [A, B];
        repeat(i => {
            const C = constructors[i & 1];
            expect(new C()).toBeInstanceOf(C);
            if (i % 100 === 0) expect(() => C()).toThrow(TypeError);
        });
    });

    test("recursion and stack overflow", () => {
        class Node {
            constructor(depth) {
                if (depth > 0) this.child = new Node(depth - 1);
                else this.child = null;
            }
        }
        class Derived extends Node {
            constructor(depth) {
                if (depth > 0) super(depth);
                else super(0);
            }
        }
        repeat(i => {
            let node = new (i & 1 ? Node : Derived)(10);
            let depth = 0;
            while (node.child) {
                node = node.child;
                depth++;
            }
            expect(depth).toBe(10);
        });
        class Deep {
            constructor() {
                new Deep();
            }
        }
        expect(() => new Deep()).toThrow(InternalError);
        expect(new Node(3).child.child.child.child).toBeNull();
    });

    test("constructors with an environment and with eval or arrows reading this", () => {
        class A {
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            constructor(x) {
                const read = () => this;
                if (x) super(x);
                else super(0);
                expect(read()).toBe(this);
            }
        }
        class C extends A {
            constructor(x) {
                if (x) super(x);
                else super(0);
                eval("this.y = 1");
            }
        }
        const constructors = [B, C];
        repeat(i => {
            const object = new constructors[i & 1](i);
            expect(object.x).toBe(i);
            if (i & 1) expect(object.y).toBe(1);
        });
    });

    test("base constructors with fields", () => {
        let count = 0;
        class A {
            a = ++count;
            #b = 2;
            b() {
                return this.#b;
            }
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            c = this.a + 1;
            constructor(x) {
                if (x) super(x);
                else super(0);
            }
        }
        const constructors = [A, B];
        repeat(i => {
            const object = new constructors[i & 1](i);
            expect(object.a).toBe(count);
            expect(object.b()).toBe(2);
            if (i & 1) expect(object.c).toBe(count + 1);
        });
    });
});

describe("literal fields of derived constructs in hot callers", () => {
    test("fields across levels are defined in order", () => {
        class A {
            a = 1;
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            b = 2;
            c;
            constructor(x) {
                super(x);
                this.y = this.b + 1;
            }
        }
        class C extends B {
            d = "d";
            b = 4;
        }
        repeat(i => {
            const b = new B(i);
            expect(Object.keys(b)).toEqual(["a", "x", "b", "c", "y"]);
            expect(b.y).toBe(3);
            expect(b.c).toBeUndefined();
            const c = new C(i);
            expect(Object.keys(c)).toEqual(["a", "x", "b", "c", "y", "d"]);
            expect(c.b).toBe(4);
            expect(c.y).toBe(3);
        });
    });

    test("fields are defined, not set, with setters on the prototype chain", () => {
        let setterCalls = 0;
        class A {
            set f(value) {
                setterCalls++;
            }
        }
        class B extends A {
            f = 1;
        }
        repeat(() => {
            const object = new B();
            expect(Object.getOwnPropertyDescriptor(object, "f")).toEqual({
                value: 1,
                writable: true,
                enumerable: true,
                configurable: true,
            });
        });
        expect(setterCalls).toBe(0);
    });

    test("parents that add different properties change the shape fields start from", () => {
        class A {
            constructor(flag) {
                if (flag) this.p = 1;
                else this.q = 2;
            }
        }
        class B extends A {
            f = 3;
        }
        repeat(i => {
            const object = new B(i % 300 === 299);
            expect(object.f).toBe(3);
            expect(Object.keys(object)).toEqual([i % 300 === 299 ? "p" : "q", "f"]);
        });
    });

    test("a parent that adds a property of the same name as a field", () => {
        class A {
            constructor(flag) {
                if (flag) Object.defineProperty(this, "f", { value: 0, writable: false, configurable: false });
            }
        }
        class B extends A {
            f = 1;
        }
        repeat(i => {
            if (i % 200 === 199) expect(() => new B(true)).toThrow(TypeError);
            else expect(new B(false).f).toBe(1);
        });
    });

    test("frozen and non-extensible objects from a parent's return override", () => {
        let mode = "none";
        class A {
            constructor() {
                if (mode === "frozen") return Object.freeze({});
                if (mode === "sealed") return Object.preventExtensions({});
            }
        }
        class B extends A {
            f = 1;
        }
        repeat(i => {
            mode = i % 100 === 50 ? "frozen" : i % 100 === 51 ? "sealed" : "none";
            if (mode === "none") expect(new B().f).toBe(1);
            else expect(() => new B()).toThrow(TypeError);
        });
    });

    test("fields with symbol names and of a subclass of a subclass", () => {
        const symbol = Symbol("s");
        class A {}
        class B extends A {
            [symbol] = 1;
            n = null;
        }
        class C extends B {
            constructor() {
                super();
                this.after = this[symbol];
            }
        }
        repeat(() => {
            const object = new C();
            expect(object[symbol]).toBe(1);
            expect(object.n).toBeNull();
            expect(object.after).toBe(1);
        });
    });
});
