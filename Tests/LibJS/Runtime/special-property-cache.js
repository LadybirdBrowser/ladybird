// Conversions of objects to primitives remember what Get of toString, valueOf, @@toPrimitive and @@toStringTag gives
// for the objects of a shape. These tests change those properties after conversions that remembered them.

const convertOften = (object, count = 50) => {
    let result;
    for (let i = 0; i < count; ++i) result = `${object}`;
    return result;
};

const withRestoredObjectPrototype = callback => {
    const keys = ["toString", "valueOf", Symbol.toPrimitive, Symbol.toStringTag];
    const descriptors = keys.map(key => Object.getOwnPropertyDescriptor(Object.prototype, key));
    try {
        callback();
    } finally {
        keys.forEach((key, index) => {
            if (descriptors[index]) Object.defineProperty(Object.prototype, key, descriptors[index]);
            else delete Object.prototype[key];
        });
    }
};

describe("replacing the values of properties of Object.prototype", () => {
    test("toString", () => {
        withRestoredObjectPrototype(() => {
            const object = { a: 1 };
            expect(convertOften(object)).toBe("[object Object]");
            Object.prototype.toString = () => "replaced";
            expect(`${object}`).toBe("replaced");
            expect(String(object)).toBe("replaced");
            expect([object, object].join()).toBe("replaced,replaced");
        });
    });

    test("valueOf", () => {
        withRestoredObjectPrototype(() => {
            const object = { b: 1 };
            expect(object + "").toBe("[object Object]");
            expect(object + "").toBe("[object Object]");
            Object.prototype.valueOf = () => 21;
            expect(object * 2).toBe(42);
            expect(object + "").toBe("21");
            expect(`${object}`).toBe("[object Object]");
        });
    });

    test("with assignments that property lookup caches make", () => {
        withRestoredObjectPrototype(() => {
            const object = { c: 1 };
            const replacements = [() => "first", () => "second"];
            const original = Object.prototype.toString;
            const results = [];
            for (let i = 0; i < 100; ++i) {
                expect(convertOften(object, 2)).toBe(i === 0 ? "[object Object]" : "second");
                Object.prototype.toString = replacements[0];
                results.push(`${object}`);
                Object.prototype.toString = replacements[1];
                results.push(`${object}`);
            }
            Object.prototype.toString = original;
            expect(`${object}`).toBe("[object Object]");
            for (let i = 0; i < results.length; ++i) expect(results[i]).toBe(i % 2 === 0 ? "first" : "second");
        });
    });

    test("with Object.assign", () => {
        withRestoredObjectPrototype(() => {
            const object = { d: 1 };
            expect(convertOften(object)).toBe("[object Object]");
            Object.assign(Object.prototype, { toString: () => "assigned" });
            expect(`${object}`).toBe("assigned");
        });
    });

    test("with Object.defineProperty", () => {
        withRestoredObjectPrototype(() => {
            const object = { e: 1 };
            expect(convertOften(object)).toBe("[object Object]");
            Object.defineProperty(Object.prototype, "toString", { value: () => "defined" });
            expect(`${object}`).toBe("defined");
            Object.defineProperty(Object.prototype, "toString", { get: () => () => "from getter" });
            expect(`${object}`).toBe("from getter");
        });
    });
});

describe("adding properties to prototypes", () => {
    test("@@toPrimitive on Object.prototype", () => {
        withRestoredObjectPrototype(() => {
            const object = { f: 1 };
            expect(convertOften(object)).toBe("[object Object]");
            Object.prototype[Symbol.toPrimitive] = hint => `primitive ${hint}`;
            expect(`${object}`).toBe("primitive string");
            expect(object + "").toBe("primitive default");
        });
    });

    test("@@toStringTag on Object.prototype", () => {
        withRestoredObjectPrototype(() => {
            const object = { g: 1 };
            expect(convertOften(object)).toBe("[object Object]");
            expect(Object.prototype.toString.call(object)).toBe("[object Object]");
            Object.prototype[Symbol.toStringTag] = "Tagged";
            expect(`${object}`).toBe("[object Tagged]");
            expect(Object.prototype.toString.call(object)).toBe("[object Tagged]");
        });
    });

    test("toString on a prototype between the object and Object.prototype", () => {
        class Base {}
        class Derived extends Base {}
        const object = new Derived();
        expect(convertOften(object)).toBe("[object Object]");
        Base.prototype.toString = () => "from Base";
        expect(`${object}`).toBe("from Base");
        Derived.prototype.toString = () => "from Derived";
        expect(`${object}`).toBe("from Derived");
        delete Derived.prototype.toString;
        expect(`${object}`).toBe("from Base");
        delete Base.prototype.toString;
        expect(`${object}`).toBe("[object Object]");
    });

    test("changing the prototype of a prototype", () => {
        const middle = {};
        const object = Object.create(middle);
        expect(convertOften(object)).toBe("[object Object]");
        Object.setPrototypeOf(middle, { toString: () => "new chain" });
        expect(`${object}`).toBe("new chain");
        Object.setPrototypeOf(middle, null);
        expect(() => `${object}`).toThrowWithMessage(TypeError, "Cannot convert object to string");
    });

    test("a prototype with many properties", () => {
        const prototype = {};
        for (let i = 0; i < 200; ++i) prototype[`property${i}`] = i;
        const object = Object.create(prototype);
        expect(convertOften(object)).toBe("[object Object]");
        prototype.toString = () => "dictionary";
        expect(`${object}`).toBe("dictionary");
        prototype.toString = () => "dictionary again";
        expect(`${object}`).toBe("dictionary again");
        delete prototype.property5;
        expect(`${object}`).toBe("dictionary again");
        delete prototype.toString;
        expect(`${object}`).toBe("[object Object]");
    });
});

describe("objects of different kinds with the same prototype", () => {
    test("errors and plain objects", () => {
        const error = new Error();
        const plain = Object.create(Error.prototype);
        for (let i = 0; i < 10; ++i) {
            expect(Object.prototype.toString.call(error)).toBe("[object Error]");
            expect(Object.prototype.toString.call(plain)).toBe("[object Object]");
        }
    });

    test("arrays and plain objects", () => {
        const array = [];
        const plain = Object.create(Array.prototype);
        for (let i = 0; i < 10; ++i) {
            expect(Object.prototype.toString.call(array)).toBe("[object Array]");
            expect(Object.prototype.toString.call(plain)).toBe("[object Object]");
        }
    });

    test("arguments objects and plain objects", () => {
        function args() {
            return arguments;
        }
        for (let i = 0; i < 10; ++i) {
            expect(String(args())).toBe("[object Arguments]");
            expect(String({})).toBe("[object Object]");
        }
    });

    test("functions", () => {
        const f = function () {};
        for (let i = 0; i < 10; ++i) expect(Object.prototype.toString.call(f)).toBe("[object Function]");
    });
});

test("own conversion properties", () => {
    const objects = [];
    for (let i = 0; i < 10; ++i) objects.push({ toString: () => `own ${i}` });
    for (let i = 0; i < 10; ++i) expect(`${objects[i]}`).toBe(`own ${i}`);
});

test("toString that is not callable", () => {
    const prototype = { toString: 42 };
    const object = Object.create(prototype);
    for (let i = 0; i < 10; ++i)
        expect(() => `${object}`).toThrowWithMessage(TypeError, "Cannot convert object to string");
    prototype.valueOf = () => 7;
    expect(`${object}`).toBe("7");
});

test("primitive wrappers", () => {
    for (let i = 0; i < 10; ++i) {
        expect(Object.prototype.toString.call(1)).toBe("[object Number]");
        expect(Object.prototype.toString.call("x")).toBe("[object String]");
        expect(Object.prototype.toString.call(true)).toBe("[object Boolean]");
        expect(Object.prototype.toString.call(Symbol())).toBe("[object Symbol]");
    }
});

describe("toJSON in JSON.stringify", () => {
    test("added to Object.prototype", () => {
        const object = { h: 1 };
        try {
            for (let i = 0; i < 10; ++i) expect(JSON.stringify(object)).toBe('{"h":1}');
            Object.prototype.toJSON = () => "added";
            expect(JSON.stringify(object)).toBe('"added"');
            expect(JSON.stringify([object, { i: 2 }])).toBe('"added"');
        } finally {
            delete Object.prototype.toJSON;
        }
        expect(JSON.stringify(object)).toBe('{"h":1}');
    });

    test("replaced on a prototype", () => {
        class Point {
            constructor(x) {
                this.x = x;
            }
            toJSON() {
                return `point ${this.x}`;
            }
        }
        const points = [new Point(1), new Point(2)];
        for (let i = 0; i < 10; ++i) expect(JSON.stringify(points)).toBe('["point 1","point 2"]');
        Point.prototype.toJSON = function () {
            return this.x * 10;
        };
        expect(JSON.stringify(points)).toBe("[10,20]");
        delete Point.prototype.toJSON;
        expect(JSON.stringify(points)).toBe('[{"x":1},{"x":2}]');
    });

    test("Date.prototype.toJSON replaced", () => {
        const original = Date.prototype.toJSON;
        const date = new Date(0);
        try {
            for (let i = 0; i < 10; ++i) expect(JSON.stringify(date)).toBe('"1970-01-01T00:00:00.000Z"');
            Date.prototype.toJSON = () => "replaced";
            expect(JSON.stringify(date)).toBe('"replaced"');
        } finally {
            Date.prototype.toJSON = original;
        }
        expect(JSON.stringify(date)).toBe('"1970-01-01T00:00:00.000Z"');
    });
});

test("@@toStringTag getter that changes @@toStringTag", () => {
    const prototype = {};
    const first = Object.create(prototype);
    const second = Object.create(prototype);
    let calls = 0;
    Object.defineProperty(prototype, Symbol.toStringTag, {
        configurable: true,
        get() {
            if (calls++ === 0) {
                Object.defineProperty(prototype, Symbol.toStringTag, { value: "X", configurable: true });
                expect(Object.prototype.toString.call(second)).toBe("[object X]");
            }
            return "Y";
        },
    });
    expect(Object.prototype.toString.call(first)).toBe("[object Y]");
    expect(Object.prototype.toString.call(first)).toBe("[object X]");
    expect(Object.prototype.toString.call(second)).toBe("[object X]");
    expect(`${first}`).toBe("[object X]");
});

describe("then and constructor in promise resolution", () => {
    const resolveOften = (makeValue, count = 50) => {
        const results = [];
        for (let i = 0; i < count; ++i)
            Promise.resolve()
                .then(() => makeValue())
                .then(value => results.push(value));
        runQueuedPromiseJobs();
        return results;
    };

    test("then added to Object.prototype", () => {
        const results = resolveOften(() => ({ a: 1 }));
        expect(results).toHaveLength(50);
        expect(results[0]).toEqual({ a: 1 });
        Object.prototype.then = function (resolve) {
            resolve("from then");
        };
        try {
            expect(resolveOften(() => ({ a: 1 }), 1)).toEqual(["from then"]);
        } finally {
            delete Object.prototype.then;
        }
        expect(resolveOften(() => ({ a: 1 }), 1)).toEqual([{ a: 1 }]);
    });

    test("Promise.prototype.then replaced", () => {
        resolveOften(() => Promise.resolve(1));
        const originalThen = Promise.prototype.then;
        let calls = 0;
        Promise.prototype.then = function (...args) {
            ++calls;
            return originalThen.apply(this, args);
        };
        try {
            let result;
            originalThen.call(
                Promise.resolve().then(() => Promise.resolve(2)),
                value => (result = value)
            );
            runQueuedPromiseJobs();
            expect(result).toBe(2);
            expect(calls).toBeGreaterThan(0);
        } finally {
            Promise.prototype.then = originalThen;
        }
    });

    test("Promise.prototype.constructor replaced", () => {
        const promise = Promise.resolve(3);
        for (let i = 0; i < 50; ++i) expect(Promise.resolve(promise)).toBe(promise);
        const originalConstructor = Promise.prototype.constructor;
        Promise.prototype.constructor = function () {};
        try {
            expect(Promise.resolve(promise)).not.toBe(promise);
        } finally {
            Promise.prototype.constructor = originalConstructor;
        }
        expect(Promise.resolve(promise)).toBe(promise);
    });
});
