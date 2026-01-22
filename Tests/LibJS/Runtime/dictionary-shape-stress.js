describe("basic dictionary transition", () => {
    test("object with few properties stays non-dictionary", () => {
        const obj = {};
        for (let i = 0; i < 10; i++) {
            obj["prop" + i] = i;
        }
        let sum = 0;
        for (let i = 0; i < 10; i++) {
            sum += obj["prop" + i];
        }
        expect(sum).toBe(45);
    });

    test("object transitions to dictionary after 64 properties", () => {
        const obj = {};
        for (let i = 0; i < 100; i++) {
            obj["prop" + i] = i;
        }
        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj["prop" + i];
        }
        expect(sum).toBe(4950);
    });

    test("property access works after dictionary transition", () => {
        const obj = {};
        for (let i = 0; i < 65; i++) {
            obj["p" + i] = i;
        }
        for (let i = 65; i < 100; i++) {
            obj["p" + i] = i;
        }
        for (let i = 0; i < 100; i++) {
            expect(obj["p" + i]).toBe(i);
        }
    });

    test("exact threshold (64 properties)", () => {
        const obj = {};
        for (let i = 0; i < 64; i++) {
            obj["prop" + i] = i;
        }
        let sum = 0;
        for (let i = 0; i < 64; i++) {
            sum += obj["prop" + i];
        }
        expect(sum).toBe(2016);

        obj.prop64 = 64;
        expect(obj.prop64).toBe(64);
        expect(obj.prop0).toBe(0);
        expect(obj.prop63).toBe(63);
    });
});

describe("IC caching with dictionary objects", () => {
    test("IC works with dictionary object (cacheable)", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        let sum = 0;
        for (let i = 0; i < 1000; i++) {
            sum += obj.p0;
        }
        expect(sum).toBe(0);

        sum = 0;
        for (let i = 0; i < 1000; i++) {
            sum += obj.p50;
        }
        expect(sum).toBe(50000);
    });

    test("IC invalidation when dictionary is modified", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj.p10;
        }
        expect(sum).toBe(1000);

        obj.p10 = 999;
        expect(obj.p10).toBe(999);

        sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj.p10;
        }
        expect(sum).toBe(99900);
    });

    test("IC invalidation when adding property to dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        expect(obj.p0).toBe(0);
        obj.newProp = 42;
        expect(obj.p0).toBe(0);
        expect(obj.newProp).toBe(42);
    });

    test("multiple dictionary objects with same property", () => {
        function getValue(obj) {
            return obj.value;
        }

        const obj1 = {};
        const obj2 = {};
        for (let i = 0; i < 70; i++) {
            obj1["p" + i] = i;
            obj2["p" + i] = i * 2;
        }
        obj1.value = 100;
        obj2.value = 200;

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(obj1);
            sum += getValue(obj2);
        }
        expect(sum).toBe(30000);
    });
});

describe("property deletion (uncacheable dictionary)", () => {
    test("delete from non-dictionary object", () => {
        const obj = { a: 1, b: 2, c: 3 };
        expect(obj.b).toBe(2);
        delete obj.b;
        expect(obj.b).toBeUndefined();
        expect(obj.a).toBe(1);
        expect(obj.c).toBe(3);
    });

    test("delete from dictionary makes it uncacheable", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        delete obj.p50;
        expect(obj.p50).toBeUndefined();
        expect(obj.p0).toBe(0);
        expect(obj.p69).toBe(69);
    });

    test("access after delete still works", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        delete obj.p10;
        delete obj.p20;
        delete obj.p30;

        let sum = 0;
        for (let i = 0; i < 70; i++) {
            if (obj["p" + i] !== undefined) {
                sum += obj["p" + i];
            }
        }
        expect(sum).toBe(2355);
    });

    test("re-add deleted property", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        delete obj.p50;
        expect(obj.p50).toBeUndefined();

        obj.p50 = 999;
        expect(obj.p50).toBe(999);
    });

    test("delete all properties from dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        for (let i = 0; i < 70; i++) {
            delete obj["p" + i];
        }

        for (let i = 0; i < 70; i++) {
            expect(obj["p" + i]).toBeUndefined();
        }

        obj.new = "value";
        expect(obj.new).toBe("value");
    });
});

describe("mixed dictionary and non-dictionary access", () => {
    test("function accessing both dictionary and non-dictionary objects", () => {
        function getValue(obj) {
            return obj.x;
        }

        const small = { x: 1 };
        const large = {};
        for (let i = 0; i < 70; i++) {
            large["p" + i] = i;
        }
        large.x = 100;

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(small);
            sum += getValue(large);
        }
        expect(sum).toBe(10100);
    });

    test("polymorphic IC with dictionary objects", () => {
        function getValue(obj) {
            return obj.value;
        }

        const objects = [];
        for (let i = 0; i < 3; i++) {
            objects.push({ value: i + 1 });
        }

        const large = {};
        for (let i = 0; i < 70; i++) {
            large["p" + i] = i;
        }
        large.value = 100;
        objects.push(large);

        let sum = 0;
        for (let round = 0; round < 100; round++) {
            for (const obj of objects) {
                sum += getValue(obj);
            }
        }
        expect(sum).toBe((1 + 2 + 3 + 100) * 100);
    });

    test("transition to dictionary during hot loop", () => {
        const obj = { value: 1 };

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj.value;
        }
        expect(sum).toBe(100);

        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj.value;
        }
        expect(sum).toBe(100);
    });
});

describe("setters with dictionary objects", () => {
    test("set property on dictionary object", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        obj.p0 = 999;
        expect(obj.p0).toBe(999);

        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i * 2;
        }

        let sum = 0;
        for (let i = 0; i < 70; i++) {
            sum += obj["p" + i];
        }
        expect(sum).toBe(4830);
    });

    test("setter IC on dictionary", () => {
        function setValue(obj, v) {
            obj.target = v;
        }

        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }
        obj.target = 0;

        for (let i = 0; i < 1000; i++) {
            setValue(obj, i);
        }
        expect(obj.target).toBe(999);
    });

    test("setter that adds property to dictionary", () => {
        function addProp(obj, name, value) {
            obj[name] = value;
        }

        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        for (let i = 70; i < 100; i++) {
            addProp(obj, "p" + i, i);
        }

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj["p" + i];
        }
        expect(sum).toBe(4950);
    });
});

describe("Object.defineProperty on dictionaries", () => {
    test("defineProperty on dictionary object", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        Object.defineProperty(obj, "defined", {
            value: 42,
            writable: true,
            enumerable: true,
            configurable: true,
        });

        expect(obj.defined).toBe(42);
    });

    test("reconfigure property on dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        Object.defineProperty(obj, "p0", {
            value: 999,
            writable: false,
        });

        expect(obj.p0).toBe(999);
        obj.p0 = 1;
        expect(obj.p0).toBe(999);
    });

    test("getter/setter on dictionary object", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        let backingValue = 0;
        Object.defineProperty(obj, "accessor", {
            get() {
                return backingValue * 2;
            },
            set(v) {
                backingValue = v;
            },
        });

        obj.accessor = 21;
        expect(obj.accessor).toBe(42);
    });
});

describe("dictionary objects with prototype chain", () => {
    test("dictionary object inheriting from prototype", () => {
        const proto = { inherited: 42 };
        const obj = Object.create(proto);

        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        expect(obj.inherited).toBe(42);
        expect(obj.p50).toBe(50);
    });

    test("prototype lookup through dictionary object", () => {
        const grandparent = { deep: "value" };
        const parent = Object.create(grandparent);

        for (let i = 0; i < 70; i++) {
            parent["p" + i] = i;
        }

        const child = Object.create(parent);

        expect(child.deep).toBe("value");
        expect(child.p50).toBe(50);
    });

    test("shadowing prototype property in dictionary", () => {
        const proto = { x: 1, y: 2 };
        const obj = Object.create(proto);

        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        expect(obj.x).toBe(1);
        obj.x = 100;
        expect(obj.x).toBe(100);
        expect(proto.x).toBe(1);
    });
});

describe("dictionary objects in different contexts", () => {
    test("dictionary object as 'this'", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }
        obj.getValue = function () {
            return this.p50;
        };

        expect(obj.getValue()).toBe(50);
    });

    test("dictionary object with method calls", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }
        obj.sum = function () {
            let s = 0;
            for (let i = 0; i < 70; i++) {
                s += this["p" + i];
            }
            return s;
        };

        expect(obj.sum()).toBe(2415);
    });

    test("dictionary in array", () => {
        const arr = [];
        for (let j = 0; j < 5; j++) {
            const obj = {};
            for (let i = 0; i < 70; i++) {
                obj["p" + i] = i + j * 100;
            }
            obj.id = j;
            arr.push(obj);
        }

        for (let j = 0; j < 5; j++) {
            expect(arr[j].id).toBe(j);
            expect(arr[j].p0).toBe(j * 100);
        }
    });

    test("dictionary object spread", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        const copy = { ...obj };

        expect(copy.p0).toBe(0);
        expect(copy.p69).toBe(69);

        copy.p0 = 999;
        expect(obj.p0).toBe(0);
    });

    test("Object.keys on dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        const keys = Object.keys(obj);
        expect(keys.length).toBe(70);
        expect(keys[0]).toBe("p0");
        expect(keys[69]).toBe("p69");
    });

    test("Object.values on dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        const values = Object.values(obj);
        expect(values.length).toBe(70);

        let sum = 0;
        for (const v of values) {
            sum += v;
        }
        expect(sum).toBe(2415);
    });

    test("Object.entries on dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        const entries = Object.entries(obj);
        expect(entries.length).toBe(70);
        expect(entries[0][0]).toBe("p0");
        expect(entries[0][1]).toBe(0);
    });

    test("for...in on dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        let count = 0;
        for (const key in obj) {
            count++;
        }
        expect(count).toBe(70);
    });
});

describe("stress tests", () => {
    test("rapid property addition/deletion", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        for (let round = 0; round < 100; round++) {
            obj["temp" + round] = round;
            if (round > 0) {
                delete obj["temp" + (round - 1)];
            }
        }

        expect(obj.temp99).toBe(99);
        expect(obj.temp50).toBeUndefined();
        expect(obj.p0).toBe(0);
    });

    test("many dictionary objects", () => {
        const objects = [];
        for (let j = 0; j < 100; j++) {
            const obj = {};
            for (let i = 0; i < 70; i++) {
                obj["p" + i] = i + j;
            }
            objects.push(obj);
        }

        let sum = 0;
        for (let j = 0; j < 100; j++) {
            sum += objects[j].p50;
        }
        expect(sum).toBe(9950);
    });

    test("dictionary with symbol properties", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        const sym = Symbol("test");
        obj[sym] = "symbol value";

        expect(obj[sym]).toBe("symbol value");
        expect(obj.p0).toBe(0);
    });

    test("dictionary with numeric string keys", () => {
        const obj = {};
        for (let i = 0; i < 100; i++) {
            obj[String(i)] = i * 2;
        }

        expect(obj[50]).toBe(100);
        expect(obj["50"]).toBe(100);

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += obj[i];
        }
        expect(sum).toBe(9900);
    });

    test("dictionary transition preserves property order", () => {
        const obj = {};
        obj.first = 1;
        obj.second = 2;
        obj.third = 3;

        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        const keys = Object.keys(obj);
        expect(keys[0]).toBe("first");
        expect(keys[1]).toBe("second");
        expect(keys[2]).toBe("third");
    });
});

describe("edge cases", () => {
    test("dictionary with frozen properties", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        Object.defineProperty(obj, "p0", {
            writable: false,
            configurable: false,
        });

        obj.p0 = 999;
        expect(obj.p0).toBe(0);
    });

    test("seal dictionary object", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        Object.seal(obj);

        obj.p0 = 999;
        expect(obj.p0).toBe(999);

        obj.newProp = 1;
        expect(obj.newProp).toBeUndefined();
    });

    test("freeze dictionary object", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        Object.freeze(obj);

        obj.p0 = 999;
        expect(obj.p0).toBe(0);

        obj.newProp = 1;
        expect(obj.newProp).toBeUndefined();
    });

    test("dictionary with __proto__ property", () => {
        const obj = Object.create(null);
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        obj.__proto__ = 42;
        expect(obj.__proto__).toBe(42);
    });

    test("hasOwnProperty on dictionary", () => {
        const proto = { inherited: 1 };
        const obj = Object.create(proto);
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        expect(obj.hasOwnProperty("p0")).toBeTrue();
        expect(obj.hasOwnProperty("p69")).toBeTrue();
        expect(obj.hasOwnProperty("inherited")).toBeFalse();
        expect("inherited" in obj).toBeTrue();
    });

    test("propertyIsEnumerable on dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        Object.defineProperty(obj, "nonEnum", {
            value: 42,
            enumerable: false,
        });

        expect(obj.propertyIsEnumerable("p0")).toBeTrue();
        expect(obj.propertyIsEnumerable("nonEnum")).toBeFalse();
    });

    test("getOwnPropertyDescriptor on dictionary", () => {
        const obj = {};
        for (let i = 0; i < 70; i++) {
            obj["p" + i] = i;
        }

        const desc = Object.getOwnPropertyDescriptor(obj, "p50");
        expect(desc.value).toBe(50);
        expect(desc.writable).toBeTrue();
        expect(desc.enumerable).toBeTrue();
        expect(desc.configurable).toBeTrue();
    });

    test("JSON.stringify dictionary", () => {
        const obj = {};
        for (let i = 0; i < 5; i++) {
            obj["p" + i] = i;
        }
        for (let i = 5; i < 70; i++) {
            obj["p" + i] = i;
        }

        const json = JSON.stringify(obj);
        const parsed = JSON.parse(json);

        expect(parsed.p0).toBe(0);
        expect(parsed.p69).toBe(69);
    });
});

describe("constructor patterns with dictionaries", () => {
    test("class instance becoming dictionary", () => {
        class BigObject {
            constructor() {
                for (let i = 0; i < 70; i++) {
                    this["p" + i] = i;
                }
            }
            sum() {
                let s = 0;
                for (let i = 0; i < 70; i++) {
                    s += this["p" + i];
                }
                return s;
            }
        }

        const obj = new BigObject();
        expect(obj.sum()).toBe(2415);
    });

    test("multiple instances with same structure", () => {
        function BigThing(id) {
            for (let i = 0; i < 70; i++) {
                this["p" + i] = i * id;
            }
            this.id = id;
        }

        const things = [];
        for (let i = 1; i <= 5; i++) {
            things.push(new BigThing(i));
        }

        expect(things[0].p10).toBe(10);
        expect(things[4].p10).toBe(50);
    });
});
