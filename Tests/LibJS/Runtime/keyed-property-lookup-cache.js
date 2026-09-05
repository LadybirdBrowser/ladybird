describe("keyed property lookup cache", () => {
    function read(object, key) {
        return object[key];
    }

    test("own properties on objects of the same shape", () => {
        const objects = [];
        for (let i = 0; i < 20; ++i) objects.push({ a: i, b: i * 2, c: i * 3 });
        for (let round = 0; round < 3; ++round) {
            for (let i = 0; i < objects.length; ++i) {
                expect(read(objects[i], "a")).toBe(i);
                expect(read(objects[i], "b")).toBe(i * 2);
                expect(read(objects[i], "c")).toBe(i * 3);
            }
        }
    });

    test("properties found on the prototype chain", () => {
        class Base {
            base() {
                return "base";
            }
        }
        class Derived extends Base {
            derived() {
                return "derived";
            }
        }
        const instance = new Derived();
        for (let i = 0; i < 5; ++i) {
            expect(read(instance, "base")()).toBe("base");
            expect(read(instance, "derived")()).toBe("derived");
        }
        Base.prototype.base = function () {
            return "replaced";
        };
        expect(read(instance, "base")()).toBe("replaced");
        instance.base = function () {
            return "shadowed";
        };
        expect(read(instance, "base")()).toBe("shadowed");
        delete instance.base;
        expect(read(instance, "base")()).toBe("replaced");
        Object.setPrototypeOf(Derived.prototype, { base: () => "swapped" });
        expect(read(instance, "base")()).toBe("swapped");
    });

    test("missing properties become present", () => {
        const object = {};
        for (let i = 0; i < 5; ++i) expect(read(object, "later")).toBeUndefined();
        object.later = 1;
        expect(read(object, "later")).toBe(1);
        const prototype = {};
        const child = Object.create(prototype);
        for (let i = 0; i < 5; ++i) expect(read(child, "inherited")).toBeUndefined();
        prototype.inherited = 2;
        expect(read(child, "inherited")).toBe(2);
        Object.setPrototypeOf(child, null);
        expect(read(child, "inherited")).toBeUndefined();
    });

    test("accessors receive the receiver as this", () => {
        const prototype = {
            get self() {
                return this;
            },
        };
        const first = Object.create(prototype);
        const second = Object.create(prototype);
        for (let i = 0; i < 5; ++i) {
            expect(read(first, "self")).toBe(first);
            expect(read(second, "self")).toBe(second);
        }
        expect(read(3, "toFixed")).toBe(Number.prototype.toFixed);
        expect(read("abc", "length")).toBe(3);
        expect(read("abc", "toUpperCase")).toBe(String.prototype.toUpperCase);
    });

    test("shape changes after the first read", () => {
        const object = { a: 1 };
        for (let i = 0; i < 5; ++i) expect(read(object, "a")).toBe(1);
        object.b = 2;
        expect(read(object, "a")).toBe(1);
        expect(read(object, "b")).toBe(2);
        delete object.a;
        expect(read(object, "a")).toBeUndefined();
        expect(read(object, "b")).toBe(2);
        object.a = 3;
        expect(read(object, "a")).toBe(3);
        Object.defineProperty(object, "b", {
            get() {
                return 4;
            },
        });
        expect(read(object, "b")).toBe(4);
    });

    test("dictionary shapes track later changes", () => {
        const object = {};
        for (let i = 0; i < 200; ++i) object["p" + i] = i;
        for (let i = 0; i < 200; ++i) expect(read(object, "p" + i)).toBe(i);
        delete object.p7;
        expect(read(object, "p7")).toBeUndefined();
        object.p7 = "back";
        expect(read(object, "p7")).toBe("back");
        for (let i = 0; i < 200; ++i) {
            if (i !== 7) expect(read(object, "p" + i)).toBe(i);
        }
    });

    test("numeric and symbol keys", () => {
        const array = [10, 20, 30];
        expect(read(array, "1")).toBe(20);
        expect(read(array, "length")).toBe(3);
        array.push(40);
        expect(read(array, "length")).toBe(4);
        const symbol = Symbol("s");
        const object = { [symbol]: "symbol" };
        for (let i = 0; i < 3; ++i) expect(read(object, symbol)).toBe("symbol");
    });

    test("proxies are never served from the cache", () => {
        let reads = 0;
        const proxy = new Proxy(
            { a: 1 },
            {
                get(target, key) {
                    ++reads;
                    return target[key];
                },
            }
        );
        for (let i = 0; i < 5; ++i) expect(read(proxy, "a")).toBe(1);
        expect(reads).toBe(5);
    });

    test("entries survive garbage collection", () => {
        const prototype = { inherited: "yes" };
        const object = Object.create(prototype);
        object.own = "own";
        expect(read(object, "inherited")).toBe("yes");
        expect(read(object, "own")).toBe("own");
        gc();
        expect(read(object, "inherited")).toBe("yes");
        expect(read(object, "own")).toBe("own");
        for (let i = 0; i < 50; ++i) {
            const short_lived = { ["k" + i]: i };
            expect(read(short_lived, "k" + i)).toBe(i);
        }
        gc();
        expect(read(object, "inherited")).toBe("yes");
    });

    test("getter that deletes itself from a unique shape", () => {
        const object = {};
        for (let i = 0; i < 1000; ++i) object["prop" + i] = i;
        for (let i = 0; i < 1000; ++i) delete object["prop" + i];
        Object.defineProperty(object, "accessor", {
            get() {
                delete object.accessor;
                return "getter";
            },
            configurable: true,
        });
        object.neighbor = "neighbor";
        expect(read(object, "accessor")).toBe("getter");
        expect(read(object, "accessor")).toBeUndefined();
        expect(read(object, "neighbor")).toBe("neighbor");
    });
});
