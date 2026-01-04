describe("basic property access", () => {
    test("basic own property get", () => {
        const obj = { x: 1, y: 2, z: 3 };
        expect(obj.x).toBe(1);
        expect(obj.y).toBe(2);
        expect(obj.z).toBe(3);
    });

    test("basic own property set", () => {
        const obj = { x: 1 };
        obj.x = 42;
        expect(obj.x).toBe(42);
    });

    test("property get in loop (IC should kick in)", () => {
        const obj = { value: 100 };
        let sum = 0;
        for (let i = 0; i < 1000; i++) {
            sum += obj.value;
        }
        expect(sum).toBe(100000);
    });

    test("property set in loop (IC should kick in)", () => {
        const obj = { counter: 0 };
        for (let i = 0; i < 1000; i++) {
            obj.counter = i;
        }
        expect(obj.counter).toBe(999);
    });
});

describe("prototype chain access", () => {
    test("single prototype chain get", () => {
        const proto = { inherited: 42 };
        const obj = Object.create(proto);
        obj.own = 1;
        expect(obj.inherited).toBe(42);
        expect(obj.own).toBe(1);
    });

    test("prototype chain get in loop", () => {
        const proto = { value: 5 };
        const obj = Object.create(proto);
        let sum = 0;
        for (let i = 0; i < 1000; i++) {
            sum += obj.value;
        }
        expect(sum).toBe(5000);
    });

    test("deep prototype chain (3 levels)", () => {
        const grandparent = { a: 1 };
        const parent = Object.create(grandparent);
        parent.b = 2;
        const child = Object.create(parent);
        child.c = 3;

        expect(child.a).toBe(1);
        expect(child.b).toBe(2);
        expect(child.c).toBe(3);
    });

    test("deep prototype chain (10 levels)", () => {
        let obj = { level: 0, base: "base" };
        for (let i = 1; i <= 10; i++) {
            obj = Object.create(obj);
            obj.level = i;
        }
        expect(obj.level).toBe(10);
        expect(obj.base).toBe("base");
    });

    test("prototype chain with null prototype", () => {
        const obj = Object.create(null);
        obj.x = 1;
        expect(obj.x).toBe(1);
        expect(obj.toString).toBeUndefined();
    });

    test("shadowing prototype property", () => {
        const proto = { x: 1 };
        const obj = Object.create(proto);
        expect(obj.x).toBe(1);
        obj.x = 2;
        expect(obj.x).toBe(2);
        expect(proto.x).toBe(1);
    });

    test("shadowing in loop", () => {
        const proto = { value: 100 };
        const objects = [];
        for (let i = 0; i < 100; i++) {
            const obj = Object.create(proto);
            if (i % 2 === 0) obj.value = i;
            objects.push(obj);
        }
        expect(objects[0].value).toBe(0);
        expect(objects[1].value).toBe(100);
        expect(objects[50].value).toBe(50);
        expect(objects[51].value).toBe(100);
    });
});

describe("shape transitions", () => {
    test("adding properties changes shape", () => {
        const obj = {};
        obj.a = 1;
        obj.b = 2;
        obj.c = 3;
        expect(obj.a).toBe(1);
        expect(obj.b).toBe(2);
        expect(obj.c).toBe(3);
    });

    test("same shape objects in loop", () => {
        const objects = [];
        for (let i = 0; i < 100; i++) {
            objects.push({ x: i, y: i * 2 });
        }
        let sum = 0;
        for (const obj of objects) {
            sum += obj.x + obj.y;
        }
        expect(sum).toBe(14850);
    });

    test("different shape objects in loop (IC polymorphism)", () => {
        const objects = [];
        for (let i = 0; i < 100; i++) {
            if (i % 3 === 0) objects.push({ x: i });
            else if (i % 3 === 1) objects.push({ x: i, y: 1 });
            else objects.push({ x: i, y: 1, z: 2 });
        }
        let sum = 0;
        for (const obj of objects) {
            sum += obj.x;
        }
        expect(sum).toBe(4950);
    });

    test("megamorphic IC (many different shapes)", () => {
        const objects = [];
        for (let i = 0; i < 100; i++) {
            const obj = {};
            for (let j = 0; j <= i % 20; j++) {
                obj["prop" + j] = j;
            }
            obj.common = i;
            objects.push(obj);
        }
        let sum = 0;
        for (const obj of objects) {
            sum += obj.common;
        }
        expect(sum).toBe(4950);
    });
});

describe("property deletion", () => {
    test("delete own property", () => {
        const obj = { x: 1, y: 2 };
        expect(obj.x).toBe(1);
        delete obj.x;
        expect(obj.x).toBeUndefined();
        expect(obj.y).toBe(2);
    });

    test("delete then re-add", () => {
        const obj = { x: 1 };
        delete obj.x;
        obj.x = 2;
        expect(obj.x).toBe(2);
    });

    test("delete exposes prototype property", () => {
        const proto = { x: "proto" };
        const obj = Object.create(proto);
        obj.x = "own";
        expect(obj.x).toBe("own");
        delete obj.x;
        expect(obj.x).toBe("proto");
    });

    test("delete in loop affects IC", () => {
        const objects = [];
        for (let i = 0; i < 100; i++) {
            const obj = { x: i, y: i * 2 };
            if (i % 2 === 0) delete obj.x;
            objects.push(obj);
        }
        let undefinedCount = 0;
        for (const obj of objects) {
            if (obj.x === undefined) undefinedCount++;
        }
        expect(undefinedCount).toBe(50);
    });
});

describe("getters and setters", () => {
    test("own getter", () => {
        const obj = {
            _x: 10,
            get x() {
                return this._x * 2;
            },
        };
        expect(obj.x).toBe(20);
    });

    test("own setter", () => {
        const obj = {
            _x: 0,
            set x(v) {
                this._x = v * 2;
            },
        };
        obj.x = 5;
        expect(obj._x).toBe(10);
    });

    test("prototype getter", () => {
        const proto = {
            get value() {
                return 42;
            },
        };
        const obj = Object.create(proto);
        expect(obj.value).toBe(42);
    });

    test("prototype setter", () => {
        const proto = {
            _value: 0,
            set value(v) {
                this._value = v;
            },
        };
        const obj = Object.create(proto);
        obj.value = 100;
        expect(obj._value).toBe(100);
        expect(proto._value).toBe(0);
    });

    test("getter in loop", () => {
        const obj = {
            _counter: 0,
            get counter() {
                return this._counter++;
            },
        };
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj.counter;
        }
        expect(sum).toBe(4950);
    });

    test("setter in loop", () => {
        const obj = {
            _value: 0,
            set value(v) {
                this._value += v;
            },
        };
        for (let i = 0; i < 100; i++) {
            obj.value = 1;
        }
        expect(obj._value).toBe(100);
    });

    test("shadowing getter with data property", () => {
        const proto = {
            get x() {
                return "getter";
            },
        };
        const obj = Object.create(proto);
        expect(obj.x).toBe("getter");
        Object.defineProperty(obj, "x", { value: "data", writable: true });
        expect(obj.x).toBe("data");
    });

    test("getter returning different values based on state", () => {
        const obj = {
            state: 0,
            get dynamic() {
                return this.state++;
            },
        };
        const results = [];
        for (let i = 0; i < 10; i++) {
            results.push(obj.dynamic);
        }
        expect(results).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    });
});

describe("Object.defineProperty", () => {
    test("defineProperty non-writable", () => {
        const obj = {};
        Object.defineProperty(obj, "x", { value: 1, writable: false });
        expect(obj.x).toBe(1);
        obj.x = 2;
        expect(obj.x).toBe(1);
    });

    test("defineProperty non-configurable", () => {
        const obj = {};
        Object.defineProperty(obj, "x", { value: 1, configurable: false });
        expect(obj.x).toBe(1);
        expect(() => {
            Object.defineProperty(obj, "x", { value: 2 });
        }).toThrow(TypeError);
    });

    test("defineProperty on prototype affects lookup", () => {
        const proto = {};
        const obj = Object.create(proto);
        expect(obj.x).toBeUndefined();
        Object.defineProperty(proto, "x", { value: 42 });
        expect(obj.x).toBe(42);
    });

    test("convert data to accessor", () => {
        const obj = { x: 1 };
        expect(obj.x).toBe(1);
        Object.defineProperty(obj, "x", {
            get() {
                return 100;
            },
            configurable: true,
        });
        expect(obj.x).toBe(100);
    });

    test("convert accessor to data", () => {
        const obj = {
            get x() {
                return 1;
            },
        };
        expect(obj.x).toBe(1);
        Object.defineProperty(obj, "x", { value: 100, configurable: true, writable: true });
        expect(obj.x).toBe(100);
    });
});

describe("prototype manipulation", () => {
    test("changing __proto__ at runtime", () => {
        const proto1 = { x: 1 };
        const proto2 = { x: 2 };
        const obj = Object.create(proto1);
        expect(obj.x).toBe(1);
        Object.setPrototypeOf(obj, proto2);
        expect(obj.x).toBe(2);
    });

    test("changing __proto__ in loop", () => {
        const proto1 = { value: 1 };
        const proto2 = { value: 2 };
        const obj = Object.create(proto1);

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            if (i === 50) Object.setPrototypeOf(obj, proto2);
            sum += obj.value;
        }
        expect(sum).toBe(150);
    });

    test("null prototype then add new prototype", () => {
        const obj = Object.create(null);
        obj.x = 1;
        expect(obj.x).toBe(1);
        expect(obj.toString).toBeUndefined();

        Object.setPrototypeOf(obj, {
            y: 2,
            toString() {
                return "obj";
            },
        });
        expect(obj.x).toBe(1);
        expect(obj.y).toBe(2);
        expect(obj.toString()).toBe("obj");
    });

    test("cyclic prototype chain attempt", () => {
        const a = {};
        const b = Object.create(a);
        expect(() => {
            Object.setPrototypeOf(a, b);
        }).toThrow(TypeError);
    });
});

describe("proxy objects", () => {
    test("proxy get trap", () => {
        const target = { x: 1 };
        const proxy = new Proxy(target, {
            get(t, prop) {
                return t[prop] * 2;
            },
        });
        expect(proxy.x).toBe(2);
    });

    test("proxy set trap", () => {
        const target = { x: 1 };
        const proxy = new Proxy(target, {
            set(t, prop, value) {
                t[prop] = value * 2;
                return true;
            },
        });
        proxy.x = 5;
        expect(target.x).toBe(10);
    });

    test("proxy in prototype chain", () => {
        const protoTarget = { inherited: 42 };
        const protoProxy = new Proxy(protoTarget, {
            get(t, prop) {
                if (prop === "inherited") return t[prop] + 1;
                return t[prop];
            },
        });
        const obj = Object.create(protoProxy);
        obj.own = 1;
        expect(obj.inherited).toBe(43);
        expect(obj.own).toBe(1);
    });

    test("proxy get trap in loop", () => {
        let getCount = 0;
        const proxy = new Proxy(
            { x: 10 },
            {
                get(t, prop) {
                    getCount++;
                    return t[prop];
                },
            }
        );
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += proxy.x;
        }
        expect(sum).toBe(1000);
        expect(getCount).toBe(100);
    });

    test("proxy that returns different values", () => {
        let counter = 0;
        const proxy = new Proxy(
            {},
            {
                get(t, prop) {
                    return counter++;
                },
            }
        );
        const results = [];
        for (let i = 0; i < 5; i++) {
            results.push(proxy.x);
        }
        expect(results).toEqual([0, 1, 2, 3, 4]);
    });
});

describe("special object types", () => {
    test("array length property", () => {
        const arr = [1, 2, 3];
        expect(arr.length).toBe(3);
        arr.length = 2;
        expect(arr.length).toBe(2);
        expect(arr[2]).toBeUndefined();
    });

    test("array index access", () => {
        const arr = [10, 20, 30];
        let sum = 0;
        for (let i = 0; i < 1000; i++) {
            sum += arr[i % 3];
        }
        expect(sum).toBe(19990);
    });

    test("array with holes", () => {
        const arr = [1, , 3];
        expect(arr[0]).toBe(1);
        expect(arr[1]).toBeUndefined();
        expect(arr[2]).toBe(3);
        expect(1 in arr).toBeFalse();
    });

    test("array prototype method access", () => {
        const arr = [1, 2, 3];
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += arr.reduce((a, b) => a + b, 0);
        }
        expect(sum).toBe(600);
    });

    test("typed array property access", () => {
        const arr = new Int32Array([1, 2, 3, 4, 5]);
        let sum = 0;
        for (let i = 0; i < 1000; i++) {
            sum += arr[i % 5];
        }
        expect(sum).toBe(3000);
    });

    test("string index access", () => {
        const str = "hello";
        let result = "";
        for (let i = 0; i < str.length; i++) {
            result += str[i];
        }
        expect(result).toBe("hello");
    });

    test("string length property", () => {
        const str = "hello";
        expect(str.length).toBe(5);
    });

    test("function properties", () => {
        function foo() {}
        foo.custom = 42;
        expect(foo.custom).toBe(42);
        expect(foo.name).toBe("foo");
        expect(typeof foo.prototype).toBe("object");
    });

    test("arguments object", () => {
        function test() {
            expect(arguments[0]).toBe(1);
            expect(arguments[1]).toBe(2);
            expect(arguments.length).toBe(3);
        }
        test(1, 2, 3);
    });
});

describe("symbol properties", () => {
    test("symbol property access", () => {
        const sym = Symbol("test");
        const obj = { [sym]: 42 };
        expect(obj[sym]).toBe(42);
    });

    test("symbol in prototype", () => {
        const sym = Symbol("inherited");
        const proto = { [sym]: "proto" };
        const obj = Object.create(proto);
        expect(obj[sym]).toBe("proto");
    });

    test("well-known symbols", () => {
        const obj = {
            [Symbol.toStringTag]: "Custom",
        };
        expect(Object.prototype.toString.call(obj)).toBe("[object Custom]");
    });

    test("symbol property iteration", () => {
        const sym1 = Symbol("a");
        const sym2 = Symbol("b");
        const obj = { [sym1]: 1, [sym2]: 2, regular: 3 };

        const symbols = Object.getOwnPropertySymbols(obj);
        expect(symbols.length).toBe(2);
        expect(obj[symbols[0]]).toBe(1);
        expect(obj[symbols[1]]).toBe(2);
    });
});

describe("computed property names", () => {
    test("computed property get", () => {
        const obj = { a: 1, b: 2, c: 3 };
        const keys = ["a", "b", "c"];
        let sum = 0;
        for (const key of keys) {
            sum += obj[key];
        }
        expect(sum).toBe(6);
    });

    test("computed property set", () => {
        const obj = {};
        const keys = ["a", "b", "c"];
        for (let i = 0; i < keys.length; i++) {
            obj[keys[i]] = i;
        }
        expect(obj.a).toBe(0);
        expect(obj.b).toBe(1);
        expect(obj.c).toBe(2);
    });

    test("computed property with numbers", () => {
        const obj = {};
        for (let i = 0; i < 100; i++) {
            obj[i] = i * 2;
        }
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj[i];
        }
        expect(sum).toBe(9900);
    });

    test("computed property coercion", () => {
        const obj = {};
        obj[1] = "one";
        obj["1"] = "one-string";
        expect(obj[1]).toBe("one-string");
        expect(obj["1"]).toBe("one-string");
    });
});

describe("constructor patterns", () => {
    test("constructor instance properties", () => {
        function Point(x, y) {
            this.x = x;
            this.y = y;
        }
        const points = [];
        for (let i = 0; i < 100; i++) {
            points.push(new Point(i, i * 2));
        }
        let sumX = 0,
            sumY = 0;
        for (const p of points) {
            sumX += p.x;
            sumY += p.y;
        }
        expect(sumX).toBe(4950);
        expect(sumY).toBe(9900);
    });

    test("constructor prototype methods", () => {
        function Counter() {
            this.value = 0;
        }
        Counter.prototype.increment = function () {
            this.value++;
        };
        Counter.prototype.get = function () {
            return this.value;
        };

        const c = new Counter();
        for (let i = 0; i < 100; i++) {
            c.increment();
        }
        expect(c.get()).toBe(100);
    });

    test("class instance properties", () => {
        class Point {
            constructor(x, y) {
                this.x = x;
                this.y = y;
            }
        }
        const points = [];
        for (let i = 0; i < 100; i++) {
            points.push(new Point(i, i * 2));
        }
        let sumX = 0,
            sumY = 0;
        for (const p of points) {
            sumX += p.x;
            sumY += p.y;
        }
        expect(sumX).toBe(4950);
        expect(sumY).toBe(9900);
    });

    test("class with getters/setters", () => {
        class Box {
            constructor(v) {
                this._v = v;
            }
            get value() {
                return this._v * 2;
            }
            set value(v) {
                this._v = v / 2;
            }
        }
        const b = new Box(10);
        expect(b.value).toBe(20);
        b.value = 100;
        expect(b._v).toBe(50);
        expect(b.value).toBe(100);
    });

    test("class inheritance", () => {
        class Animal {
            constructor(name) {
                this.name = name;
            }
            speak() {
                return "...";
            }
        }
        class Dog extends Animal {
            speak() {
                return "woof";
            }
        }
        class Cat extends Animal {
            speak() {
                return "meow";
            }
        }

        const animals = [new Dog("Rex"), new Cat("Whiskers"), new Dog("Buddy")];
        const sounds = animals.map(a => a.speak());
        expect(sounds).toEqual(["woof", "meow", "woof"]);
    });

    test("class static properties", () => {
        class Counter {
            static count = 0;
            constructor() {
                Counter.count++;
            }
        }
        for (let i = 0; i < 10; i++) {
            new Counter();
        }
        expect(Counter.count).toBe(10);
    });
});

describe("edge cases", () => {
    test("property named __proto__", () => {
        const obj = Object.create(null);
        obj.__proto__ = 42;
        expect(obj.__proto__).toBe(42);
    });

    test("property named constructor", () => {
        const obj = { constructor: 42 };
        expect(obj.constructor).toBe(42);
    });

    test("numeric string keys vs number keys", () => {
        const obj = {};
        obj["0"] = "string";
        expect(obj[0]).toBe("string");
        obj[0] = "number";
        expect(obj["0"]).toBe("number");
    });

    test("very long property name", () => {
        const longName = "a".repeat(1000);
        const obj = { [longName]: 42 };
        expect(obj[longName]).toBe(42);
    });

    test("property access on primitives", () => {
        expect((42).toString()).toBe("42");
        expect("hello".toUpperCase()).toBe("HELLO");
        expect(true.toString()).toBe("true");
    });

    test("undefined/null property access", () => {
        expect(() => null.x).toThrow(TypeError);
        expect(() => undefined.x).toThrow(TypeError);
    });

    test("hasOwnProperty vs in operator", () => {
        const proto = { inherited: 1 };
        const obj = Object.create(proto);
        obj.own = 2;

        expect("own" in obj).toBeTrue();
        expect("inherited" in obj).toBeTrue();
        expect(obj.hasOwnProperty("own")).toBeTrue();
        expect(obj.hasOwnProperty("inherited")).toBeFalse();
    });

    test("Object.keys vs Object.getOwnPropertyNames", () => {
        const obj = {};
        Object.defineProperty(obj, "hidden", { value: 1, enumerable: false });
        obj.visible = 2;

        expect(Object.keys(obj)).toEqual(["visible"]);
        expect(Object.getOwnPropertyNames(obj).sort()).toEqual(["hidden", "visible"]);
    });

    test("frozen object property access", () => {
        const obj = Object.freeze({ x: 1, y: 2 });
        expect(obj.x).toBe(1);
        obj.x = 100;
        expect(obj.x).toBe(1);
        obj.z = 3;
        expect(obj.z).toBeUndefined();
    });

    test("sealed object property access", () => {
        const obj = Object.seal({ x: 1 });
        expect(obj.x).toBe(1);
        obj.x = 100;
        expect(obj.x).toBe(100);
        obj.y = 2;
        expect(obj.y).toBeUndefined();
    });

    test("non-extensible object", () => {
        const obj = Object.preventExtensions({ x: 1 });
        obj.x = 2;
        expect(obj.x).toBe(2);
        obj.y = 3;
        expect(obj.y).toBeUndefined();
    });
});

describe("prototype mutations during access", () => {
    test("modify prototype during iteration", () => {
        const proto = { a: 1, b: 2 };
        const obj = Object.create(proto);

        const values = [];
        for (const key in obj) {
            values.push(obj[key]);
            if (key === "a") proto.c = 3;
        }
        expect(obj.c).toBe(3);
    });

    test("delete prototype property during access", () => {
        const proto = { x: 1 };
        const obj = Object.create(proto);

        expect(obj.x).toBe(1);
        delete proto.x;
        expect(obj.x).toBeUndefined();
    });

    test("add own property shadows prototype during loop", () => {
        const proto = { value: "proto" };
        const objects = [];
        for (let i = 0; i < 10; i++) {
            objects.push(Object.create(proto));
        }

        const results = [];
        for (let i = 0; i < objects.length; i++) {
            results.push(objects[i].value);
            if (i === 4) objects[5].value = "own";
        }
        expect(results).toEqual([
            "proto",
            "proto",
            "proto",
            "proto",
            "proto",
            "own",
            "proto",
            "proto",
            "proto",
            "proto",
        ]);
    });
});

describe("mixed access patterns", () => {
    test("alternating get and set", () => {
        const obj = { x: 0 };
        for (let i = 0; i < 100; i++) {
            obj.x = obj.x + 1;
        }
        expect(obj.x).toBe(100);
    });

    test("multiple objects same shape alternating access", () => {
        const a = { x: 1, y: 2 };
        const b = { x: 10, y: 20 };
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += i % 2 === 0 ? a.x + a.y : b.x + b.y;
        }
        expect(sum).toBe(1650);
    });

    test("property access through variable", () => {
        const obj = { a: 1, b: 2, c: 3 };
        const props = ["a", "b", "c", "a", "b", "c"];
        let sum = 0;
        for (const p of props) {
            sum += obj[p];
        }
        expect(sum).toBe(12);
    });

    test("nested object access", () => {
        const obj = {
            level1: {
                level2: {
                    level3: {
                        value: 42,
                    },
                },
            },
        };
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj.level1.level2.level3.value;
        }
        expect(sum).toBe(4200);
    });

    test("optional chaining", () => {
        const obj = { a: { b: { c: 42 } } };
        expect(obj?.a?.b?.c).toBe(42);
        expect(obj?.x?.y?.z).toBeUndefined();
        expect(null?.x).toBeUndefined();
    });
});

describe("IC invalidation scenarios", () => {
    test("IC invalidation by adding property to prototype", () => {
        const proto = {};
        const objects = [];
        for (let i = 0; i < 10; i++) {
            objects.push(Object.create(proto));
        }

        for (let i = 0; i < 100; i++) {
            for (const obj of objects) {
                obj.x;
            }
        }

        proto.x = 42;

        let sum = 0;
        for (const obj of objects) {
            sum += obj.x;
        }
        expect(sum).toBe(420);
    });

    test("IC invalidation by deleting property from prototype", () => {
        const proto = { x: 42 };
        const obj = Object.create(proto);

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj.x;
        }
        expect(sum).toBe(4200);

        delete proto.x;

        expect(obj.x).toBeUndefined();
    });

    test("IC with changing object shapes", () => {
        function getValue(obj) {
            return obj.value;
        }

        const obj1 = { value: 1 };
        const obj2 = { a: 0, value: 2 };
        const obj3 = { a: 0, b: 0, value: 3 };

        expect(getValue(obj1)).toBe(1);
        expect(getValue(obj2)).toBe(2);
        expect(getValue(obj3)).toBe(3);
        expect(getValue(obj1)).toBe(1);
        expect(getValue(obj2)).toBe(2);
    });
});

describe("global object access", () => {
    test("global property access", () => {
        globalThis.testGlobal = 42;
        expect(testGlobal).toBe(42);
        delete globalThis.testGlobal;
    });

    test("global property in loop", () => {
        globalThis.loopCounter = 0;
        for (let i = 0; i < 100; i++) {
            loopCounter++;
        }
        expect(loopCounter).toBe(100);
        delete globalThis.loopCounter;
    });
});

describe("Reflect API", () => {
    test("Reflect.get", () => {
        const obj = { x: 42 };
        expect(Reflect.get(obj, "x")).toBe(42);
    });

    test("Reflect.set", () => {
        const obj = { x: 1 };
        Reflect.set(obj, "x", 42);
        expect(obj.x).toBe(42);
    });

    test("Reflect.get with receiver", () => {
        const proto = {
            get x() {
                return this.y * 2;
            },
        };
        const obj = Object.create(proto);
        obj.y = 10;
        expect(Reflect.get(proto, "x", obj)).toBe(20);
    });

    test("Reflect.has", () => {
        const proto = { inherited: 1 };
        const obj = Object.create(proto);
        obj.own = 2;
        expect(Reflect.has(obj, "own")).toBeTrue();
        expect(Reflect.has(obj, "inherited")).toBeTrue();
        expect(Reflect.has(obj, "missing")).toBeFalse();
    });
});

describe("Map/WeakMap", () => {
    test("Map property-like access pattern", () => {
        const map = new Map();
        map.set("x", 1);
        map.set("y", 2);

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += map.get("x") + map.get("y");
        }
        expect(sum).toBe(300);
    });

    test("WeakMap with object keys", () => {
        const wm = new WeakMap();
        const keys = [];
        for (let i = 0; i < 10; i++) {
            const key = {};
            keys.push(key);
            wm.set(key, i);
        }

        let sum = 0;
        for (const key of keys) {
            sum += wm.get(key);
        }
        expect(sum).toBe(45);
    });
});

describe("super property access", () => {
    test("super property get", () => {
        class Parent {
            getValue() {
                return 42;
            }
        }
        class Child extends Parent {
            getValue() {
                return super.getValue() * 2;
            }
        }
        const c = new Child();
        expect(c.getValue()).toBe(84);
    });

    test("super property get in loop", () => {
        class Parent {
            getValue() {
                return 1;
            }
        }
        class Child extends Parent {
            getValue() {
                return super.getValue() + 1;
            }
        }
        const c = new Child();
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += c.getValue();
        }
        expect(sum).toBe(200);
    });

    test("super with getter", () => {
        class Parent {
            get value() {
                return 10;
            }
        }
        class Child extends Parent {
            get value() {
                return super.value * 2;
            }
        }
        const c = new Child();
        expect(c.value).toBe(20);
    });
});

describe("polymorphic IC boundary tests", () => {
    test("monomorphic IC (1 shape)", () => {
        function getValue(obj) {
            return obj.x;
        }
        const objects = [];
        for (let i = 0; i < 100; i++) {
            objects.push({ x: i });
        }
        let sum = 0;
        for (const obj of objects) {
            sum += getValue(obj);
        }
        expect(sum).toBe(4950);
    });

    test("dimorphic IC (2 shapes)", () => {
        function getValue(obj) {
            return obj.x;
        }
        const shape1 = [];
        const shape2 = [];
        for (let i = 0; i < 50; i++) {
            shape1.push({ x: i });
            shape2.push({ x: i, y: 0 });
        }
        let sum = 0;
        for (let i = 0; i < 50; i++) {
            sum += getValue(shape1[i]);
            sum += getValue(shape2[i]);
        }
        expect(sum).toBe(2450);
    });

    test("trimorphic IC (3 shapes)", () => {
        function getValue(obj) {
            return obj.x;
        }
        const objects = [];
        for (let i = 0; i < 99; i++) {
            if (i % 3 === 0) objects.push({ x: i });
            else if (i % 3 === 1) objects.push({ x: i, y: 0 });
            else objects.push({ x: i, y: 0, z: 0 });
        }
        let sum = 0;
        for (const obj of objects) {
            sum += getValue(obj);
        }
        expect(sum).toBe(4851);
    });

    test("tetramorphic IC (4 shapes - at limit)", () => {
        function getValue(obj) {
            return obj.x;
        }
        const objects = [];
        for (let i = 0; i < 100; i++) {
            if (i % 4 === 0) objects.push({ x: i });
            else if (i % 4 === 1) objects.push({ x: i, a: 0 });
            else if (i % 4 === 2) objects.push({ x: i, b: 0 });
            else objects.push({ x: i, c: 0 });
        }
        let sum = 0;
        for (const obj of objects) {
            sum += getValue(obj);
        }
        expect(sum).toBe(4950);
    });

    test("pentamorphic IC (5 shapes - exceeds limit)", () => {
        function getValue(obj) {
            return obj.x;
        }
        const objects = [];
        for (let i = 0; i < 100; i++) {
            if (i % 5 === 0) objects.push({ x: i });
            else if (i % 5 === 1) objects.push({ x: i, a: 0 });
            else if (i % 5 === 2) objects.push({ x: i, b: 0 });
            else if (i % 5 === 3) objects.push({ x: i, c: 0 });
            else objects.push({ x: i, d: 0 });
        }
        let sum = 0;
        for (const obj of objects) {
            sum += getValue(obj);
        }
        expect(sum).toBe(4950);
    });

    test("megamorphic IC (20 shapes)", () => {
        function getValue(obj) {
            return obj.x;
        }
        const objects = [];
        for (let i = 0; i < 100; i++) {
            const obj = { x: i };
            for (let j = 0; j < i % 20; j++) {
                obj["prop" + j] = j;
            }
            objects.push(obj);
        }
        let sum = 0;
        for (const obj of objects) {
            sum += getValue(obj);
        }
        expect(sum).toBe(4950);
    });

    test("polymorphic IC with prototype differences", () => {
        function getValue(obj) {
            return obj.x;
        }
        const proto1 = { type: 1 };
        const proto2 = { type: 2 };
        const proto3 = { type: 3 };
        const proto4 = { type: 4 };

        const objects = [];
        for (let i = 0; i < 100; i++) {
            let obj;
            if (i % 4 === 0) obj = Object.create(proto1);
            else if (i % 4 === 1) obj = Object.create(proto2);
            else if (i % 4 === 2) obj = Object.create(proto3);
            else obj = Object.create(proto4);
            obj.x = i;
            objects.push(obj);
        }
        let sum = 0;
        for (const obj of objects) {
            sum += getValue(obj);
        }
        expect(sum).toBe(4950);
    });

    test("polymorphic setter IC (4 shapes)", () => {
        function setValue(obj, v) {
            obj.x = v;
        }
        const objects = [];
        for (let i = 0; i < 100; i++) {
            if (i % 4 === 0) objects.push({});
            else if (i % 4 === 1) objects.push({ a: 0 });
            else if (i % 4 === 2) objects.push({ b: 0 });
            else objects.push({ c: 0 });
        }
        for (let i = 0; i < objects.length; i++) {
            setValue(objects[i], i);
        }
        let sum = 0;
        for (const obj of objects) {
            sum += obj.x;
        }
        expect(sum).toBe(4950);
    });

    test("polymorphic IC interleaved access patterns", () => {
        function getValue(obj) {
            return obj.value;
        }

        const a = { value: 1 };
        const b = { value: 2, extra1: 0 };
        const c = { value: 3, extra1: 0, extra2: 0 };
        const d = { value: 4, extra1: 0, extra2: 0, extra3: 0 };

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(a);
            sum += getValue(b);
            sum += getValue(c);
            sum += getValue(d);
        }
        expect(sum).toBe(1000);

        for (let i = 0; i < 100; i++) {
            sum += getValue(d);
            sum += getValue(c);
            sum += getValue(b);
            sum += getValue(a);
        }
        expect(sum).toBe(2000);
    });

    test("polymorphic IC with shape transition during access", () => {
        function getValue(obj) {
            return obj.x;
        }

        const objects = [];
        for (let i = 0; i < 100; i++) {
            objects.push({ x: i });
        }

        let sum = 0;
        for (const obj of objects) {
            sum += getValue(obj);
        }
        expect(sum).toBe(4950);

        for (let i = 0; i < 25; i++) {
            objects[i].a = 0;
        }
        for (let i = 25; i < 50; i++) {
            objects[i].b = 0;
        }
        for (let i = 50; i < 75; i++) {
            objects[i].c = 0;
        }

        sum = 0;
        for (const obj of objects) {
            sum += getValue(obj);
        }
        expect(sum).toBe(4950);
    });

    test("polymorphic IC crossing 4-shape boundary", () => {
        function getValue(obj) {
            return obj.x;
        }

        const objects = [{ x: 1 }, { x: 2, a: 0 }, { x: 3, b: 0 }, { x: 4, c: 0 }];

        for (let i = 0; i < 100; i++) {
            for (const obj of objects) {
                getValue(obj);
            }
        }

        objects.push({ x: 5, d: 0 });

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            for (const obj of objects) {
                sum += getValue(obj);
            }
        }
        expect(sum).toBe(1500);
    });

    test("polymorphic IC with getters mixed in", () => {
        function getValue(obj) {
            return obj.x;
        }

        const regular1 = { x: 1 };
        const regular2 = { x: 2, y: 0 };
        const withGetter1 = {
            get x() {
                return 3;
            },
        };
        const withGetter2 = {
            get x() {
                return 4;
            },
            y: 0,
        };

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(regular1);
            sum += getValue(regular2);
            sum += getValue(withGetter1);
            sum += getValue(withGetter2);
        }
        expect(sum).toBe(1000);
    });

    test("polymorphic IC with null prototype objects", () => {
        function getValue(obj) {
            return obj.x;
        }

        const regular = { x: 1 };
        const nullProto = Object.create(null);
        nullProto.x = 2;
        const nullProto2 = Object.create(null);
        nullProto2.x = 3;
        nullProto2.y = 0;

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(regular);
            sum += getValue(nullProto);
            sum += getValue(nullProto2);
        }
        expect(sum).toBe(600);
    });

    test("polymorphic IC stress - rapid shape changes", () => {
        function getValue(obj) {
            return obj.x;
        }

        let sum = 0;
        for (let i = 0; i < 1000; i++) {
            const obj = { x: i };
            for (let j = 0; j < i % 10; j++) {
                obj["p" + j] = 0;
            }
            sum += getValue(obj);
        }
        expect(sum).toBe(499500);
    });

    test("polymorphic IC - same property different positions", () => {
        function getValue(obj) {
            return obj.target;
        }

        const a = { target: 1 };
        const b = { a: 0, target: 2 };
        const c = { a: 0, b: 0, target: 3 };
        const d = { a: 0, b: 0, c: 0, target: 4 };

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(a) + getValue(b) + getValue(c) + getValue(d);
        }
        expect(sum).toBe(1000);
    });

    test("polymorphic IC - property in prototype at different depths", () => {
        function getValue(obj) {
            return obj.inherited;
        }

        const proto1 = { inherited: 1 };
        const proto2a = Object.create({ inherited: 2 });
        const proto2b = { other: 0 };
        proto2b.inherited = 3;

        const obj1 = Object.create(proto1);
        const obj2 = Object.create(proto2a);
        const obj3 = Object.create(proto2b);
        const obj4 = { inherited: 4 };

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(obj1) + getValue(obj2) + getValue(obj3) + getValue(obj4);
        }
        expect(sum).toBe(1000);
    });

    test("polymorphic setter IC with shape transitions", () => {
        function setValue(obj, v) {
            obj.x = v;
        }

        const objects = [];
        for (let i = 0; i < 100; i++) {
            objects.push({});
        }

        for (let i = 0; i < 100; i++) {
            setValue(objects[i], i);
        }

        let sum = 0;
        for (const obj of objects) {
            sum += obj.x;
        }
        expect(sum).toBe(4950);

        for (let i = 0; i < 100; i++) {
            objects[i]["extra" + (i % 4)] = 0;
        }

        for (let i = 0; i < 100; i++) {
            setValue(objects[i], i * 2);
        }

        sum = 0;
        for (const obj of objects) {
            sum += obj.x;
        }
        expect(sum).toBe(9900);
    });

    test("polymorphic IC with frozen/sealed objects", () => {
        function getValue(obj) {
            return obj.x;
        }

        const regular = { x: 1 };
        const frozen = Object.freeze({ x: 2 });
        const sealed = Object.seal({ x: 3 });
        const nonExtensible = Object.preventExtensions({ x: 4 });

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(regular);
            sum += getValue(frozen);
            sum += getValue(sealed);
            sum += getValue(nonExtensible);
        }
        expect(sum).toBe(1000);
    });

    test("polymorphic IC - array vs object", () => {
        function getFirst(obj) {
            return obj[0];
        }

        const arr = [42];
        const obj = { 0: 42 };
        const typedArr = new Int32Array([42]);

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getFirst(arr);
            sum += getFirst(obj);
            sum += getFirst(typedArr);
        }
        expect(sum).toBe(12600);
    });

    test("IC cache thrashing - alternating hot/cold", () => {
        function getValue(obj) {
            return obj.x;
        }

        const shapes = [];
        for (let i = 0; i < 8; i++) {
            const obj = { x: i + 1 };
            for (let j = 0; j < i; j++) {
                obj["prop" + j] = 0;
            }
            shapes.push(obj);
        }

        let sum = 0;
        for (let i = 0; i < 800; i++) {
            sum += getValue(shapes[i % 8]);
        }
        expect(sum).toBe(3600);
    });

    test("IC with computed property becoming constant", () => {
        function getValue(obj, prop) {
            return obj[prop];
        }

        const obj = { a: 1, b: 2, c: 3, d: 4 };

        let sum = 0;
        const props = ["a", "b", "c", "d"];
        for (let i = 0; i < 100; i++) {
            sum += getValue(obj, props[i % 4]);
        }
        expect(sum).toBe(250);

        sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(obj, "a");
        }
        expect(sum).toBe(100);
    });
});
