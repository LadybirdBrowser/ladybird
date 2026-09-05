test("length is 2", () => {
    expect(Object.assign).toHaveLength(2);
});

describe("property access ordering", () => {
    test("reads current values after a target setter", () => {
        const source = { a: 1, b: 2 };
        const target = {
            set a(value) {
                expect(value).toBe(1);
                source.b = 99;
            },
        };
        Object.assign(target, source);
        expect(target.b).toBe(99);
    });

    test("snapshots keys but observes deletion and enumerability changes", () => {
        const source = { a: 1, b: 2, c: 3 };
        Object.defineProperty(source, "hidden", { value: 4, configurable: true });
        const target = {
            set a(value) {
                delete source.b;
                Object.defineProperty(source, "c", { enumerable: false });
                Object.defineProperty(source, "hidden", { enumerable: true });
                source.added = 5;
            },
        };
        Object.assign(target, source);
        expect(target).not.toHaveProperty("b");
        expect(target).not.toHaveProperty("c");
        expect(target).not.toHaveProperty("added");
        expect(target.hidden).toBe(4);
    });

    test("observes data properties replaced by accessors with unchanged attributes", () => {
        const source = { a: 1, b: 2 };
        let getterCalls = 0;
        const target = {
            set a(value) {
                Object.defineProperty(source, "b", {
                    get() {
                        ++getterCalls;
                        return 42;
                    },
                    set(value) {},
                    enumerable: true,
                    configurable: true,
                });
            },
        };
        Object.assign(target, source);
        expect(target.b).toBe(42);
        expect(getterCalls).toBe(1);
    });

    test("source getters can change later properties", () => {
        const source = {
            get a() {
                delete this.b;
                this.c = 30;
                this.d = 40;
                return 10;
            },
            b: 2,
            c: 3,
        };
        expect(Object.assign({}, source)).toEqual({ a: 10, c: 30 });
    });

    test("does not invoke non-enumerable getters", () => {
        const source = { a: 1 };
        Object.defineProperty(source, "hidden", {
            get() {
                throw new Error("Unexpected getter");
            },
        });
        expect(Object.assign({}, source)).toEqual({ a: 1 });
    });

    test("preserves key order when a setter deletes and reinserts a key", () => {
        const source = { a: 1, b: 2, c: 3 };
        const order = [];
        const target = new Proxy(
            {},
            {
                set(target, key, value) {
                    order.push(key);
                    if (key === "a") {
                        delete source.b;
                        source.b = 20;
                    }
                    target[key] = value;
                    return true;
                },
            }
        );
        Object.assign(target, source);
        expect(order).toEqual(["a", "b", "c"]);
        expect(target.b).toBe(20);
    });

    test("uses integer, string, then symbol key order", () => {
        const first = Symbol("first");
        const second = Symbol("second");
        const source = { [first]: 1, b: 2, 10: 3, [second]: 4, a: 5, 2: 6 };
        const order = [];
        Object.assign(
            new Proxy(
                {},
                {
                    set(target, key) {
                        order.push(key);
                        return true;
                    },
                }
            ),
            source
        );
        expect(order).toEqual(["2", "10", "b", "a", first, second]);
    });

    test("preserves source proxy trap order", () => {
        const log = [];
        const source = new Proxy(
            { a: 1, b: 2 },
            {
                ownKeys(target) {
                    log.push("keys");
                    return ["b", "a"];
                },
                getOwnPropertyDescriptor(target, key) {
                    log.push("descriptor " + key);
                    return Reflect.getOwnPropertyDescriptor(target, key);
                },
                get(target, key) {
                    log.push("get " + key);
                    return target[key];
                },
            }
        );
        Object.assign(
            new Proxy(
                {},
                {
                    set(target, key) {
                        log.push("set " + key);
                        return true;
                    },
                }
            ),
            source
        );
        expect(log).toEqual(["keys", "descriptor b", "get b", "set b", "descriptor a", "get a", "set a"]);
    });
});

describe("target Set behavior", () => {
    test("invokes inherited setters", () => {
        const source = { a: 1, b: 2 };
        const target = Object.create({
            set a(value) {
                expect(this).toBe(target);
                source.b = 99;
            },
        });
        Object.assign(target, source);
        expect(Object.hasOwn(target, "a")).toBeFalse();
        expect(target.b).toBe(99);
    });

    test("preserves target attributes and normalizes new property attributes", () => {
        const target = {};
        Object.defineProperty(target, "a", { value: 0, writable: true });
        Object.assign(target, Object.freeze({ a: 1, b: 2 }));
        expect(Object.getOwnPropertyDescriptor(target, "a")).toEqual({
            value: 1,
            writable: true,
            enumerable: false,
            configurable: false,
        });
        expect(Object.getOwnPropertyDescriptor(target, "b")).toEqual({
            value: 2,
            writable: true,
            enumerable: true,
            configurable: true,
        });
    });

    test("keeps earlier writes when a later property is not writable", () => {
        for (const inherited of [false, true]) {
            const holder = {};
            Object.defineProperty(holder, "b", { value: 0 });
            const target = inherited ? Object.create(holder) : holder;
            expect(() => Object.assign(target, { a: 1, b: 2, c: 3 })).toThrow(TypeError);
            expect(target.a).toBe(1);
            expect(target.b).toBe(0);
            expect(target).not.toHaveProperty("c");
        }
    });

    test("keeps earlier writes when a later getter throws", () => {
        const target = {};
        expect(() =>
            Object.assign(target, {
                a: 1,
                get b() {
                    throw new Error("stop");
                },
                c: 3,
            })
        ).toThrowWithMessage(Error, "stop");
        expect(target).toEqual({ a: 1 });
    });

    test("handles non-extensible and frozen targets", () => {
        const target = Object.preventExtensions({ a: 0 });
        expect(() => Object.assign(target, { a: 1, b: 2 })).toThrow(TypeError);
        expect(target.a).toBe(1);
        expect(() => Object.assign(Object.freeze({ a: 0 }), { a: 1 })).toThrow(TypeError);
        expect(Object.assign(Object.freeze({}), {})).toEqual({});
    });

    test("invokes the inherited __proto__ setter", () => {
        const prototype = { inherited: 42 };
        const target = Object.assign({}, { ["__proto__"]: prototype, own: 1 });
        expect(Object.getPrototypeOf(target)).toBe(prototype);
        expect(Object.hasOwn(target, "__proto__")).toBeFalse();
        expect(target.own).toBe(1);
    });

    test("self assignment and multiple sources", () => {
        const target = { a: 1, b: 2 };
        expect(Object.assign(target, target, null, { b: 3 }, undefined, { c: 4 })).toBe(target);
        expect(target).toEqual({ a: 1, b: 3, c: 4 });
    });

    test("handles dictionary sources and transition to dictionary during copying", () => {
        const source = { a: 1, b: 2 };
        const target = {
            set a(value) {
                for (let i = 0; i < 100; ++i) source["key" + i] = i;
                source.b = 99;
            },
        };
        Object.assign(target, source);
        expect(target.b).toBe(99);
        expect(target).not.toHaveProperty("key0");
        const copy = Object.assign({}, source);
        expect(copy.b).toBe(99);
        expect(copy.key99).toBe(99);
    });

    test("handles null prototypes and exotic sources and targets", () => {
        const source = Object.create(null);
        source.a = 1;
        expect(Object.assign(Object.create(null), source).a).toBe(1);
        expect(Object.assign({}, "ab")).toEqual({ 0: "a", 1: "b" });
        expect(Object.assign({}, new Uint8Array([3, 4]))).toEqual({ 0: 3, 1: 4 });
        const array = [1, 2, 3];
        Object.assign(array, { length: 1 });
        expect(array).toEqual([1]);
    });

    test("reads mapped arguments after a target setter changes a parameter", () => {
        const copy = Function(
            "value",
            `
            const source = arguments;
            return Object.assign({ set first(value) {} }, source);
        `
        )(42);
        expect(copy[0]).toBe(42);

        const result = Function(
            "a",
            "b",
            `
            const source = arguments;
            const target = { set 0(value) { b = 99; } };
            return Object.assign(target, source);
        `
        )(1, 2);
        expect(result[1]).toBe(99);
    });

    test("copies enumerable function properties", () => {
        function source() {}
        source.a = 1;
        source.b = 2;
        expect(Object.assign({}, source)).toEqual({ a: 1, b: 2 });
    });
});

describe("errors", () => {
    test("first argument must coercible to object", () => {
        expect(() => {
            Object.assign(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
        expect(() => {
            Object.assign(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });
});

describe("normal behavior", () => {
    test("returns first argument coerced to object", () => {
        const o = {};
        expect(Object.assign(o)).toBe(o);
        expect(Object.assign(o, {})).toBe(o);
        expect(Object.assign(42)).toEqual(new Number(42));
    });

    test("alters first argument object if sources are given", () => {
        const o = { foo: 0 };
        expect(Object.assign(o, { foo: 1 })).toBe(o);
        expect(o).toEqual({ foo: 1 });
    });

    test("merges objects", () => {
        const s = Symbol();
        expect(Object.assign({}, { foo: 0, bar: "baz" }, { [s]: [1, 2, 3] }, { foo: 1 }, { [42]: "test" })).toEqual({
            foo: 1,
            bar: "baz",
            [s]: [1, 2, 3],
            42: "test",
        });
    });
});
