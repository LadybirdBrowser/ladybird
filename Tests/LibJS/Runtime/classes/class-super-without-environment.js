describe("this in derived constructors", () => {
    test("reading this before super() throws", () => {
        class A {}
        class B extends A {
            constructor() {
                this.x = 1;
                super();
            }
        }
        expect(() => new B()).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("super property access before super() throws", () => {
        class A {
            m() {}
        }
        class B extends A {
            constructor() {
                super.m();
                super();
            }
        }
        expect(() => new B()).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("calling super() twice throws after the second call runs the super constructor", () => {
        let calls = 0;
        class A {
            constructor() {
                calls++;
            }
        }
        class B extends A {
            constructor() {
                super();
                super();
            }
        }
        expect(() => new B()).toThrowWithMessage(ReferenceError, "|this| is already initialized");
        expect(calls).toBe(2);
    });

    test("returning without calling super() throws", () => {
        class A {}
        class B extends A {
            constructor() {}
        }
        expect(() => new B()).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("return override", () => {
        class A {}
        const object = {};
        class ReturnsObject extends A {
            constructor() {
                return object;
            }
        }
        expect(new ReturnsObject()).toBe(object);

        class ReturnsObjectAfterSuper extends A {
            constructor() {
                super();
                return object;
            }
        }
        expect(new ReturnsObjectAfterSuper()).toBe(object);

        class ReturnsNumber extends A {
            constructor() {
                super();
                return 1;
            }
        }
        expect(() => new ReturnsNumber()).toThrow(TypeError);

        class ReturnsUndefined extends A {
            constructor() {
                super();
                this.x = 1;
                return undefined;
            }
        }
        expect(new ReturnsUndefined().x).toBe(1);
    });

    test("super() in a branch and this in a catch handler", () => {
        class A {}
        class B extends A {
            constructor(callSuper) {
                let first;
                try {
                    first = this;
                } catch (e) {
                    first = e.constructor;
                }
                if (callSuper) super();
                this.first = first;
            }
        }
        expect(new B(true).first).toBe(ReferenceError);
        expect(() => new B(false)).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("a closure that does not read this", () => {
        class A {}
        class B extends A {
            constructor(x) {
                const get = () => x;
                super();
                this.get = get;
            }
        }
        expect(new B(5).get()).toBe(5);
    });
});

describe("arrow functions and eval in derived constructors", () => {
    test("an arrow function calls super()", () => {
        class A {
            constructor(x) {
                this.x = x;
            }
        }
        class B extends A {
            constructor() {
                const callSuper = () => super(7);
                callSuper();
                this.y = this.x + 1;
            }
        }
        const b = new B();
        expect(b.x).toBe(7);
        expect(b.y).toBe(8);
    });

    test("an arrow function reads this before and after super()", () => {
        class A {}
        class B extends A {
            constructor() {
                const getThis = () => this;
                expect(getThis).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
                super();
                this.self = getThis();
            }
        }
        const b = new B();
        expect(b.self).toBe(b);
    });

    test("eval calls super() and reads this", () => {
        class A {}
        class B extends A {
            constructor() {
                expect(() => eval("this")).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
                eval("super()");
                this.self = eval("this");
            }
        }
        const b = new B();
        expect(b.self).toBe(b);
    });

    test("eval inside an arrow function reads this", () => {
        class A {}
        class B extends A {
            constructor() {
                super();
                this.self = (() => eval("this"))();
            }
        }
        const b = new B();
        expect(b.self).toBe(b);
    });
});

describe("class elements initialized by super()", () => {
    test("fields and private methods", () => {
        class A {
            constructor() {
                this.order = ["A"];
            }
        }
        class B extends A {
            field = this.order.push("field");
            #method() {
                return "private";
            }
            constructor() {
                super();
                this.order.push("constructor");
                this.result = this.#method();
            }
        }
        const b = new B();
        expect(b.order).toEqual(["A", "field", "constructor"]);
        expect(b.result).toBe("private");
    });

    test("default derived constructor", () => {
        class A {
            constructor(...args) {
                this.args = args;
                this.target = new.target;
            }
        }
        class B extends A {
            field = 1;
        }
        const b = new B(1, 2, 3);
        expect(b.args).toEqual([1, 2, 3]);
        expect(b.target).toBe(B);
        expect(b.field).toBe(1);

        class C {}
        expect(new C()).toBeInstanceOf(C);
    });
});

describe("super properties", () => {
    class Base {
        get value() {
            return "base " + this.name;
        }
        set value(v) {
            this.stored = v;
        }
        method(x) {
            return [this.name, x];
        }
        static staticMethod() {
            return "static " + this.name;
        }
    }

    test("get, call, computed, set and delete in instance methods", () => {
        class Derived extends Base {
            constructor() {
                super();
                this.name = "derived";
            }
            get() {
                return super.value;
            }
            call(x) {
                return super.method(x);
            }
            computed(key) {
                return super[key];
            }
            set(v) {
                super.value = v;
                super.other = 1;
                return this.other;
            }
            delete() {
                delete super.method;
            }
        }
        const d = new Derived();
        expect(d.get()).toBe("base derived");
        expect(d.call(2)).toEqual(["derived", 2]);
        expect(d.computed("value")).toBe("base derived");
        expect(d.set(5)).toBe(1);
        expect(d.stored).toBe(5);
        expect(() => d.delete()).toThrow(ReferenceError);
    });

    test("static methods, getters and setters", () => {
        class Derived extends Base {
            static staticMethod() {
                return super.staticMethod() + "!";
            }
            get value() {
                return super.value + "?";
            }
            set value(v) {
                super.value = v * 2;
            }
        }
        expect(Derived.staticMethod()).toBe("static Derived!");
        const d = new Derived();
        d.name = "d";
        expect(d.value).toBe("base d?");
        d.value = 3;
        expect(d.stored).toBe(6);
    });

    test("object literal methods", () => {
        const proto = {
            greet() {
                return "hi " + this.name;
            },
        };
        const object = {
            __proto__: proto,
            name: "object",
            greet() {
                return super.greet() + "!";
            },
        };
        expect(object.greet()).toBe("hi object!");
    });

    test("generator and async methods suspend and resume", async () => {
        class Derived extends Base {
            *generator() {
                yield super.method(1);
                yield super.method(2);
            }
            async asyncMethod() {
                const first = super.method(1);
                await null;
                return [first, super.method(2)];
            }
        }
        const d = new Derived();
        d.name = "g";
        expect([...d.generator()]).toEqual([
            ["g", 1],
            ["g", 2],
        ]);
        expect(await d.asyncMethod()).toEqual([
            ["g", 1],
            ["g", 2],
        ]);
    });

    test("a method called on many receivers", () => {
        class Derived extends Base {
            method(x) {
                return super.method(x + 1)[1];
            }
        }
        const d = new Derived();
        let sum = 0;
        for (let i = 0; i < 1000; i++) sum += d.method(i);
        expect(sum).toBe(500500);
    });
});

describe("new.target", () => {
    test("in functions", () => {
        function f() {
            return new.target;
        }
        expect(f()).toBeUndefined();
        expect(new f()).toBe(f);
        function G() {}
        expect(Reflect.construct(f, [], G)).toBe(G);
    });

    test("in constructors with Reflect.construct", () => {
        let seen;
        class A {
            constructor() {
                seen = new.target;
            }
        }
        class B extends A {
            constructor() {
                const before = new.target;
                super();
                this.before = before;
                this.after = new.target;
            }
        }
        function Other() {}
        const b = Reflect.construct(B, [], Other);
        expect(b.before).toBe(Other);
        expect(b.after).toBe(Other);
        expect(seen).toBe(Other);
        expect(Object.getPrototypeOf(b)).toBe(Other.prototype);
    });

    test("in generators and async functions", async () => {
        function* generator() {
            yield new.target;
        }
        expect(generator().next().value).toBeUndefined();
        async function asyncFunction() {
            await null;
            return new.target;
        }
        expect(await asyncFunction()).toBeUndefined();
    });
});
