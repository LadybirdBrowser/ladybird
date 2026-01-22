describe("basic object literals", () => {
    test("simple object literal", () => {
        const obj = { a: 1, b: 2, c: 3 };
        expect(obj.a).toBe(1);
        expect(obj.b).toBe(2);
        expect(obj.c).toBe(3);
    });

    test("object literal with string keys", () => {
        const obj = { foo: 1, "bar-baz": 2, "123abc": 3 };
        expect(obj.foo).toBe(1);
        expect(obj["bar-baz"]).toBe(2);
        expect(obj["123abc"]).toBe(3);
    });

    test("object literal with numeric keys", () => {
        const obj = { 0: "zero", 1: "one", 42: "forty-two" };
        expect(obj[0]).toBe("zero");
        expect(obj[1]).toBe("one");
        expect(obj[42]).toBe("forty-two");
    });

    test("mixed property types", () => {
        const obj = { a: 1, 0: "indexed", quoted: true };
        expect(obj.a).toBe(1);
        expect(obj[0]).toBe("indexed");
        expect(obj.quoted).toBe(true);
    });
});

describe("object literals in loops (cache testing)", () => {
    test("creating many identical-shape objects in a loop", () => {
        const objects = [];
        for (let i = 0; i < 1000; i++) {
            objects.push({ x: i, y: i * 2, z: i * 3 });
        }
        expect(objects[0].x).toBe(0);
        expect(objects[0].y).toBe(0);
        expect(objects[0].z).toBe(0);
        expect(objects[500].x).toBe(500);
        expect(objects[500].y).toBe(1000);
        expect(objects[500].z).toBe(1500);
        expect(objects[999].x).toBe(999);
    });

    test("objects with same shape should have same structure", () => {
        function createPoint(x, y) {
            return { x: x, y: y };
        }
        const points = [];
        for (let i = 0; i < 100; i++) {
            points.push(createPoint(i, i * 2));
        }
        let allCorrect = true;
        for (let i = 0; i < 100; i++) {
            if (points[i].x !== i || points[i].y !== i * 2) {
                allCorrect = false;
                break;
            }
        }
        expect(allCorrect).toBeTrue();
    });

    test("nested object literals in loops", () => {
        const objects = [];
        for (let i = 0; i < 100; i++) {
            objects.push({
                id: i,
                nested: {
                    value: i * 10,
                    deep: {
                        data: i * 100,
                    },
                },
            });
        }
        expect(objects[50].id).toBe(50);
        expect(objects[50].nested.value).toBe(500);
        expect(objects[50].nested.deep.data).toBe(5000);
    });

    test("factory function creating objects", () => {
        function createUser(id, name) {
            return { id: id, name: name, active: true };
        }
        const users = [];
        for (let i = 0; i < 1000; i++) {
            users.push(createUser(i, "User" + i));
        }
        expect(users[0].id).toBe(0);
        expect(users[999].id).toBe(999);
        expect(users[500].name).toBe("User500");
    });

    test("object literals as map entries", () => {
        const entries = [];
        for (let i = 0; i < 100; i++) {
            entries.push({ key: "k" + i, value: i * 10 });
        }
        expect(entries[0].key).toBe("k0");
        expect(entries[0].value).toBe(0);
        expect(entries[50].key).toBe("k50");
        expect(entries[50].value).toBe(500);
    });

    test("inline object literal in function call", () => {
        function process(options) {
            return options.a + options.b;
        }
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += process({ a: i, b: i * 2 });
        }
        expect(sum).toBe(14850);
    });

    test("object destructuring assignment target", () => {
        const results = [];
        for (let i = 0; i < 100; i++) {
            const { x, y } = { x: i, y: i * 2 };
            results.push(x + y);
        }
        expect(results[0]).toBe(0);
        expect(results[50]).toBe(150);
        expect(results[99]).toBe(297);
    });
});

describe("computed properties", () => {
    test("basic computed properties", () => {
        const key = "dynamicKey";
        const obj = { [key]: "value", ["computed" + "Key"]: 42 };
        expect(obj.dynamicKey).toBe("value");
        expect(obj.computedKey).toBe(42);
    });

    test("computed properties with expressions", () => {
        const obj = {
            [1 + 2]: "three",
            [Math.max(5, 10)]: "ten",
            [`template${1}key`]: "templateValue",
        };
        expect(obj[3]).toBe("three");
        expect(obj[10]).toBe("ten");
        expect(obj.template1key).toBe("templateValue");
    });

    test("computed properties in loops", () => {
        const objects = [];
        for (let i = 0; i < 10; i++) {
            objects.push({
                ["prop" + i]: i,
                static: "same",
            });
        }
        expect(objects[0].prop0).toBe(0);
        expect(objects[5].prop5).toBe(5);
        expect(objects[9].prop9).toBe(9);
        expect(objects[5].static).toBe("same");
    });
});

describe("property shorthand", () => {
    test("shorthand properties", () => {
        const x = 1,
            y = 2,
            z = 3;
        const obj = { x, y, z };
        expect(obj.x).toBe(1);
        expect(obj.y).toBe(2);
        expect(obj.z).toBe(3);
    });

    test("mixed shorthand and regular properties", () => {
        const name = "Alice";
        const age = 30;
        const obj = { name, age, city: "NYC", active: true };
        expect(obj.name).toBe("Alice");
        expect(obj.age).toBe(30);
        expect(obj.city).toBe("NYC");
        expect(obj.active).toBe(true);
    });
});

describe("method shorthand", () => {
    test("method shorthand", () => {
        const obj = {
            getValue() {
                return 42;
            },
            add(a, b) {
                return a + b;
            },
        };
        expect(obj.getValue()).toBe(42);
        expect(obj.add(3, 4)).toBe(7);
    });

    test("computed method names", () => {
        const methodName = "dynamicMethod";
        const obj = {
            [methodName]() {
                return "dynamic";
            },
            ["get" + "Value"]() {
                return 123;
            },
        };
        expect(obj.dynamicMethod()).toBe("dynamic");
        expect(obj.getValue()).toBe(123);
    });
});

describe("getters and setters", () => {
    test("basic getter", () => {
        const obj = {
            _value: 10,
            get value() {
                return this._value * 2;
            },
        };
        expect(obj.value).toBe(20);
    });

    test("basic setter", () => {
        const obj = {
            _value: 0,
            set value(v) {
                this._value = v * 2;
            },
        };
        obj.value = 5;
        expect(obj._value).toBe(10);
    });

    test("getter and setter together", () => {
        const obj = {
            _data: [],
            get length() {
                return this._data.length;
            },
            set length(v) {
                this._data.length = v;
            },
        };
        obj._data = [1, 2, 3];
        expect(obj.length).toBe(3);
        obj.length = 1;
        expect(obj._data.length).toBe(1);
    });

    test("computed getter/setter names", () => {
        const propName = "computedProp";
        const obj = {
            _store: {},
            get [propName]() {
                return this._store[propName];
            },
            set [propName](v) {
                this._store[propName] = v;
            },
        };
        obj.computedProp = "test";
        expect(obj.computedProp).toBe("test");
    });
});

describe("spread properties", () => {
    test("basic spread", () => {
        const source = { a: 1, b: 2 };
        const obj = { ...source, c: 3 };
        expect(obj.a).toBe(1);
        expect(obj.b).toBe(2);
        expect(obj.c).toBe(3);
    });

    test("spread overwriting", () => {
        const defaults = { a: 1, b: 2, c: 3 };
        const overrides = { b: 20, d: 40 };
        const obj = { ...defaults, ...overrides };
        expect(obj.a).toBe(1);
        expect(obj.b).toBe(20);
        expect(obj.c).toBe(3);
        expect(obj.d).toBe(40);
    });

    test("spread with explicit properties", () => {
        const source = { a: 1, b: 2 };
        const obj = { a: 100, ...source, b: 200 };
        expect(obj.a).toBe(1);
        expect(obj.b).toBe(200);
    });

    test("spread null and undefined (should be no-op)", () => {
        const obj = { ...null, ...undefined, a: 1 };
        expect(obj.a).toBe(1);
        expect(Object.keys(obj).length).toBe(1);
    });
});

describe("__proto__ property", () => {
    test("__proto__ sets prototype", () => {
        const proto = { inherited: 42 };
        const obj = { __proto__: proto, own: 1 };
        expect(obj.inherited).toBe(42);
        expect(obj.own).toBe(1);
        expect(Object.getPrototypeOf(obj)).toBe(proto);
    });

    test("__proto__ as computed property (doesn't set prototype)", () => {
        const proto = { inherited: 42 };
        const obj = { ["__proto__"]: proto };
        expect(obj.__proto__).toBe(proto);
        expect(Object.getPrototypeOf(obj)).not.toBe(proto);
    });
});

describe("duplicate keys", () => {
    test("duplicate keys (last wins)", () => {
        const obj = { a: 1, a: 2, a: 3 };
        expect(obj.a).toBe(3);
        expect(Object.keys(obj).length).toBe(1);
    });

    test("duplicate with different types", () => {
        const obj = { 1: "numeric", 1: "string" };
        expect(obj[1]).toBe("string");
    });
});

describe("property order", () => {
    test("integer keys come first (in numeric order)", () => {
        const obj = { b: 2, 2: "two", a: 1, 1: "one", c: 3, 10: "ten" };
        const keys = Object.keys(obj);
        // Integer keys should be first, in numeric order
        expect(keys[0]).toBe("1");
        expect(keys[1]).toBe("2");
        expect(keys[2]).toBe("10");
        // Then string keys in insertion order
        expect(keys[3]).toBe("b");
        expect(keys[4]).toBe("a");
        expect(keys[5]).toBe("c");
    });
});

describe("edge cases", () => {
    test("empty object literal", () => {
        const obj = {};
        expect(Object.keys(obj).length).toBe(0);
        expect(typeof obj).toBe("object");
    });

    test("object with only getter", () => {
        const obj = {
            get x() {
                return 42;
            },
        };
        expect(obj.x).toBe(42);
        obj.x = 100; // Should be silently ignored in non-strict mode
        expect(obj.x).toBe(42);
    });

    test("very long property names", () => {
        const longKey = "a".repeat(1000);
        const obj = { [longKey]: "value" };
        expect(obj[longKey]).toBe("value");
    });

    test("unicode property names", () => {
        const obj = { "\u{1F600}": "emoji", "caf\u00E9": "coffee", "\u4E2D\u6587": "chinese" };
        expect(obj["\u{1F600}"]).toBe("emoji");
        expect(obj["caf\u00E9"]).toBe("coffee");
        expect(obj["\u4E2D\u6587"]).toBe("chinese");
    });

    test("property with undefined value", () => {
        const obj = { a: undefined, b: null, c: 0, d: "", e: false };
        expect(obj.a).toBeUndefined();
        expect(obj.b).toBeNull();
        expect(obj.c).toBe(0);
        expect(obj.d).toBe("");
        expect(obj.e).toBeFalse();
        expect("a" in obj).toBeTrue();
    });

    test("self-referencing (captures old value)", () => {
        let obj = { old: "value" };
        obj = { ref: obj };
        expect(obj.ref).toEqual({ old: "value" });
    });
});

describe("objects with many properties", () => {
    test("object with many properties added dynamically", () => {
        const obj = {};
        for (let i = 0; i < 100; i++) {
            obj["prop" + i] = i;
        }
        expect(obj.prop0).toBe(0);
        expect(obj.prop50).toBe(50);
        expect(obj.prop99).toBe(99);
        expect(Object.keys(obj).length).toBe(100);
    });

    test("object literal with many properties at once", () => {
        const obj = {
            a1: 1,
            a2: 2,
            a3: 3,
            a4: 4,
            a5: 5,
            a6: 6,
            a7: 7,
            a8: 8,
            a9: 9,
            a10: 10,
            b1: 11,
            b2: 12,
            b3: 13,
            b4: 14,
            b5: 15,
            b6: 16,
            b7: 17,
            b8: 18,
            b9: 19,
            b10: 20,
            c1: 21,
            c2: 22,
            c3: 23,
            c4: 24,
            c5: 25,
            c6: 26,
            c7: 27,
            c8: 28,
            c9: 29,
            c10: 30,
        };
        expect(obj.a1).toBe(1);
        expect(obj.b5).toBe(15);
        expect(obj.c10).toBe(30);
        expect(Object.keys(obj).length).toBe(30);
    });
});

describe("prototype and inheritance", () => {
    test("object literal with null prototype", () => {
        const obj = { __proto__: null, a: 1 };
        expect(obj.a).toBe(1);
        expect(Object.getPrototypeOf(obj)).toBeNull();
        expect(obj.toString).toBeUndefined();
    });

    test("chained prototype in object literals", () => {
        const grandparent = { level: 0 };
        const parent = { __proto__: grandparent, level: 1 };
        const child = { __proto__: parent, level: 2 };
        expect(child.level).toBe(2);
        expect(Object.getPrototypeOf(child).level).toBe(1);
        expect(Object.getPrototypeOf(Object.getPrototypeOf(child)).level).toBe(0);
    });
});

describe("cache invalidation scenarios", () => {
    test("different property values don't affect shape cache", () => {
        const objects = [];
        for (let i = 0; i < 100; i++) {
            // Same property names, different values
            objects.push({ x: i % 2 === 0 ? "even" : "odd", y: i * Math.random() });
        }
        expect(objects[0].x).toBe("even");
        expect(objects[1].x).toBe("odd");
    });

    test("symbol properties", () => {
        const sym = Symbol("test");
        const objects = [];
        for (let i = 0; i < 100; i++) {
            objects.push({ [sym]: i, regular: "value" });
        }
        expect(objects[50][sym]).toBe(50);
        expect(objects[50].regular).toBe("value");
    });

    test("mixed symbol and string properties", () => {
        const sym1 = Symbol("a");
        const sym2 = Symbol("b");
        const obj = { [sym1]: 1, regular: 2, [sym2]: 3 };
        expect(obj[sym1]).toBe(1);
        expect(obj.regular).toBe(2);
        expect(obj[sym2]).toBe(3);
    });
});
