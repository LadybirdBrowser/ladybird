// Converting plain objects to primitives has a fast path for objects that use the
// conversions of an unchanged Object.prototype. Everything else must still behave
// like the specification says.

describe("plain objects", () => {
    test("convert to [object Object] for every hint", () => {
        const object = { a: 1 };
        expect(String(object)).toBe("[object Object]");
        expect(`${object}`).toBe("[object Object]");
        expect(object + "").toBe("[object Object]");
        expect([object, object].join()).toBe("[object Object],[object Object]");
        expect(Number.isNaN(+object)).toBeTrue();
        expect(String({})).toBe("[object Object]");
    });

    test("own toString, valueOf, @@toPrimitive and @@toStringTag are used", () => {
        expect(String({ toString: () => "own" })).toBe("own");
        expect({ valueOf: () => 42 } + 1).toBe(43);
        expect(String({ valueOf: () => 42 })).toBe("[object Object]");
        expect(String({ [Symbol.toPrimitive]: hint => hint })).toBe("string");
        expect({ [Symbol.toPrimitive]: hint => hint } + "").toBe("default");
        expect(String({ [Symbol.toStringTag]: "Tagged" })).toBe("[object Tagged]");
        expect(
            String({
                get toString() {
                    return () => "getter";
                },
            })
        ).toBe("getter");
    });

    test("objects with other builtin tags and Object.prototype as their prototype", () => {
        const date = new Date(0);
        Object.setPrototypeOf(date, Object.prototype);
        expect(String(date)).toBe("[object Date]");
        const regexp = /a/;
        Object.setPrototypeOf(regexp, Object.prototype);
        expect(String(regexp)).toBe("[object RegExp]");
        const array = [1, 2];
        Object.setPrototypeOf(array, Object.prototype);
        expect(String(array)).toBe("[object Array]");
        const error = new Error("x");
        Object.setPrototypeOf(error, Object.prototype);
        expect(String(error)).toBe("[object Error]");
        function args() {
            return arguments;
        }
        expect(String(args(1))).toBe("[object Arguments]");
        expect(String(Object.create(Object.prototype))).toBe("[object Object]");
        expect(() => String(Object.create(null))).toThrow(TypeError);
    });

    test("changes to Object.prototype are seen", () => {
        const object = { a: 1 };
        const originalToString = Object.prototype.toString;
        const originalValueOf = Object.prototype.valueOf;
        try {
            expect(String(object)).toBe("[object Object]");
            Object.prototype.toString = () => "patched";
            expect(String(object)).toBe("patched");
            expect([object].join()).toBe("patched");
            Object.prototype.toString = originalToString;
            expect(String(object)).toBe("[object Object]");

            Object.prototype.valueOf = () => 7;
            expect(object + 1).toBe(8);
            expect(String(object)).toBe("[object Object]");
            Object.prototype.valueOf = originalValueOf;
            expect(object + "").toBe("[object Object]");

            Object.prototype[Symbol.toStringTag] = "Proto";
            expect(String(object)).toBe("[object Proto]");
            delete Object.prototype[Symbol.toStringTag];
            expect(String(object)).toBe("[object Object]");

            Object.prototype[Symbol.toPrimitive] = () => "primitive";
            expect(String(object)).toBe("primitive");
            delete Object.prototype[Symbol.toPrimitive];
            expect(String(object)).toBe("[object Object]");

            let getterCalls = 0;
            Object.defineProperty(Object.prototype, Symbol.toStringTag, {
                get() {
                    ++getterCalls;
                    return "Getter";
                },
                configurable: true,
            });
            expect(String(object)).toBe("[object Getter]");
            expect(getterCalls).toBe(1);
            delete Object.prototype[Symbol.toStringTag];
        } finally {
            Object.prototype.toString = originalToString;
            Object.prototype.valueOf = originalValueOf;
            delete Object.prototype[Symbol.toStringTag];
            delete Object.prototype[Symbol.toPrimitive];
        }
    });

    test("proxies with Object.prototype as their target's prototype", () => {
        const proxy = new Proxy(
            {},
            {
                get(target, key) {
                    if (key === "toString") return () => "from proxy";
                    return Reflect.get(target, key);
                },
            }
        );
        expect(String(proxy)).toBe("from proxy");
    });
});

describe("objects further down the prototype chain", () => {
    test("instances of classes without conversions of their own", () => {
        class A {}
        class B extends A {}
        expect(String(new A())).toBe("[object Object]");
        expect(`${new B()}`).toBe("[object Object]");
        expect([new B(), new A()].join("|")).toBe("[object Object]|[object Object]");
    });

    test("conversions on a prototype in between are used", () => {
        class WithToString {
            toString() {
                return "class";
            }
        }
        class Derived extends WithToString {}
        expect(String(new Derived())).toBe("class");

        const prototype = {};
        const object = Object.create(prototype);
        expect(String(object)).toBe("[object Object]");
        prototype.valueOf = () => 5;
        expect(object * 2).toBe(10);
        prototype[Symbol.toStringTag] = "Middle";
        expect(String(object)).toBe("[object Middle]");
    });

    test("long prototype chains and proxies on the chain", () => {
        let object = {};
        for (let i = 0; i < 10; ++i) object = Object.create(object);
        expect(String(object)).toBe("[object Object]");

        const proxy = new Proxy(Object.create(Object.prototype), {
            get(target, key, receiver) {
                if (key === Symbol.toStringTag) return "FromProxy";
                return Reflect.get(target, key, receiver);
            },
        });
        expect(String(Object.create(proxy))).toBe("[object FromProxy]");
    });
});

describe("objects with the same properties", () => {
    test("adding a conversion to an object gives it its own shape", () => {
        const make = () => ({ a: 1, b: 2 });
        const first = make();
        const second = make();
        expect(`${first}`).toBe("[object Object]");
        second.toString = () => "second";
        expect(`${second}`).toBe("second");
        expect(`${make()}`).toBe("[object Object]");
        const third = make();
        third[Symbol.toStringTag] = "Third";
        expect(`${third}`).toBe("[object Third]");
        expect(`${make()}`).toBe("[object Object]");
    });

    test("a prototype that gains a conversion after it was checked", () => {
        class Base {}
        const instance = new Base();
        expect(`${instance}`).toBe("[object Object]");
        Base.prototype.valueOf = () => 42;
        expect(instance + 1).toBe(43);
        delete Base.prototype.valueOf;
        expect(`${instance}`).toBe("[object Object]");
        Base.prototype[Symbol.toPrimitive] = () => "primitive";
        expect(`${instance}`).toBe("primitive");
    });

    test("objects with many properties", () => {
        const object = {};
        for (let i = 0; i < 100; ++i) object[`key${i}`] = i;
        expect(`${object}`).toBe("[object Object]");
        object.toString = () => "many";
        expect(`${object}`).toBe("many");
        delete object.toString;
        expect(`${object}`).toBe("[object Object]");
    });
});
