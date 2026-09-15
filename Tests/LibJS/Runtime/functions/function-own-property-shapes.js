function expectLengthAndName(fn, length, name) {
    const lengthDescriptor = Object.getOwnPropertyDescriptor(fn, "length");
    expect(lengthDescriptor.value).toBe(length);
    expect(lengthDescriptor.writable).toBeFalse();
    expect(lengthDescriptor.enumerable).toBeFalse();
    expect(lengthDescriptor.configurable).toBeTrue();

    const nameDescriptor = Object.getOwnPropertyDescriptor(fn, "name");
    expect(nameDescriptor.value).toBe(name);
    expect(nameDescriptor.writable).toBeFalse();
    expect(nameDescriptor.enumerable).toBeFalse();
    expect(nameDescriptor.configurable).toBeTrue();
}

function expectPrototypeProperty(fn, expectedPrototypeOfPrototype) {
    const descriptor = Object.getOwnPropertyDescriptor(fn, "prototype");
    expect(descriptor.writable).toBeTrue();
    expect(descriptor.enumerable).toBeFalse();
    expect(descriptor.configurable).toBeFalse();
    expect(Object.getPrototypeOf(descriptor.value)).toBe(expectedPrototypeOfPrototype);
    expect(Reflect.ownKeys(descriptor.value)).toEqual([]);
}

const GeneratorFunctionPrototype = Object.getPrototypeOf(function* () {});
const AsyncFunctionPrototype = Object.getPrototypeOf(async function () {});
const AsyncGeneratorFunctionPrototype = Object.getPrototypeOf(async function* () {});

describe("own property keys and descriptors", () => {
    test("arrow function", () => {
        const arrow = (a, b) => a + b;
        expect(Reflect.ownKeys(arrow)).toEqual(["length", "name"]);
        expect(Object.getOwnPropertyNames(arrow)).toEqual(["length", "name"]);
        expect(Object.keys(Object.getOwnPropertyDescriptors(arrow))).toEqual(["length", "name"]);
        expectLengthAndName(arrow, 2, "arrow");
        expect(Object.getPrototypeOf(arrow)).toBe(Function.prototype);
        expect(arrow.hasOwnProperty("prototype")).toBeFalse();
        expect(arrow(1, 2)).toBe(3);
    });

    test("async arrow function", () => {
        const asyncArrow = async (a, b, c) => a;
        expect(Reflect.ownKeys(asyncArrow)).toEqual(["length", "name"]);
        expect(Object.keys(Object.getOwnPropertyDescriptors(asyncArrow))).toEqual(["length", "name"]);
        expectLengthAndName(asyncArrow, 3, "asyncArrow");
        expect(Object.getPrototypeOf(asyncArrow)).toBe(AsyncFunctionPrototype);
        expect(asyncArrow.hasOwnProperty("prototype")).toBeFalse();
    });

    test("async function", () => {
        async function asyncFunction(a) {}
        expect(Reflect.ownKeys(asyncFunction)).toEqual(["length", "name"]);
        expect(Object.getOwnPropertyNames(asyncFunction)).toEqual(["length", "name"]);
        expectLengthAndName(asyncFunction, 1, "asyncFunction");
        expect(Object.getPrototypeOf(asyncFunction)).toBe(AsyncFunctionPrototype);
        expect(asyncFunction.hasOwnProperty("prototype")).toBeFalse();
    });

    test("generator function", () => {
        function* generator(a, b) {
            yield a;
        }
        expect(Reflect.ownKeys(generator)).toEqual(["length", "name", "prototype"]);
        expect(Object.keys(Object.getOwnPropertyDescriptors(generator))).toEqual(["length", "name", "prototype"]);
        expectLengthAndName(generator, 2, "generator");
        expect(Object.getPrototypeOf(generator)).toBe(GeneratorFunctionPrototype);
        expectPrototypeProperty(generator, GeneratorFunctionPrototype.prototype);
        expect(generator(5).next().value).toBe(5);
        expect(Object.getPrototypeOf(generator())).toBe(generator.prototype);

        // Each generator function gets its own prototype object.
        const makeGenerator = () => function* () {};
        expect(makeGenerator().prototype).not.toBe(makeGenerator().prototype);
    });

    test("async generator function", () => {
        async function* asyncGenerator() {}
        expect(Reflect.ownKeys(asyncGenerator)).toEqual(["length", "name", "prototype"]);
        expect(Object.getOwnPropertyNames(asyncGenerator)).toEqual(["length", "name", "prototype"]);
        expectLengthAndName(asyncGenerator, 0, "asyncGenerator");
        expect(Object.getPrototypeOf(asyncGenerator)).toBe(AsyncGeneratorFunctionPrototype);
        expectPrototypeProperty(asyncGenerator, AsyncGeneratorFunctionPrototype.prototype);
        expect(Object.getPrototypeOf(asyncGenerator())).toBe(asyncGenerator.prototype);
    });

    test("object literal methods and accessors", () => {
        const object = {
            method(a) {},
            async asyncMethod() {},
            *generatorMethod() {},
            async *asyncGeneratorMethod() {},
            get getter() {
                return 1;
            },
            set setter(value) {},
            ["computed" + "Arrow"]: () => {},
            [Symbol.iterator]: async () => {},
        };
        expect(Reflect.ownKeys(object.method)).toEqual(["length", "name"]);
        expectLengthAndName(object.method, 1, "method");
        expect(Reflect.ownKeys(object.asyncMethod)).toEqual(["length", "name"]);
        expectLengthAndName(object.asyncMethod, 0, "asyncMethod");
        expect(Reflect.ownKeys(object.generatorMethod)).toEqual(["length", "name", "prototype"]);
        expectPrototypeProperty(object.generatorMethod, GeneratorFunctionPrototype.prototype);
        expect(Reflect.ownKeys(object.asyncGeneratorMethod)).toEqual(["length", "name", "prototype"]);
        expectPrototypeProperty(object.asyncGeneratorMethod, AsyncGeneratorFunctionPrototype.prototype);

        const getter = Object.getOwnPropertyDescriptor(object, "getter").get;
        expect(Reflect.ownKeys(getter)).toEqual(["length", "name"]);
        expectLengthAndName(getter, 0, "get getter");
        const setter = Object.getOwnPropertyDescriptor(object, "setter").set;
        expect(Reflect.ownKeys(setter)).toEqual(["length", "name"]);
        expectLengthAndName(setter, 1, "set setter");

        expect(Reflect.ownKeys(object.computedArrow)).toEqual(["length", "name"]);
        expectLengthAndName(object.computedArrow, 0, "computedArrow");
        expect(Reflect.ownKeys(object[Symbol.iterator])).toEqual(["length", "name"]);
        expectLengthAndName(object[Symbol.iterator], 0, "[Symbol.iterator]");
    });

    test("class methods", () => {
        class C {
            static async staticAsync() {}
            async asyncMethod(a, b) {}
            *generatorMethod() {}
            static staticField = () => {};
            field = async () => {};
        }
        expect(Reflect.ownKeys(C.staticAsync)).toEqual(["length", "name"]);
        expectLengthAndName(C.staticAsync, 0, "staticAsync");
        expect(Reflect.ownKeys(C.prototype.asyncMethod)).toEqual(["length", "name"]);
        expectLengthAndName(C.prototype.asyncMethod, 2, "asyncMethod");
        expect(Reflect.ownKeys(C.prototype.generatorMethod)).toEqual(["length", "name", "prototype"]);
        expectLengthAndName(C.prototype.generatorMethod, 0, "generatorMethod");
        expect(Reflect.ownKeys(C.staticField)).toEqual(["length", "name"]);
        expectLengthAndName(C.staticField, 0, "staticField");
        const instance = new C();
        expect(Reflect.ownKeys(instance.field)).toEqual(["length", "name"]);
        expectLengthAndName(instance.field, 0, "field");
    });

    test("anonymous functions get their name from the binding", () => {
        let arrow;
        arrow = () => {};
        const asyncArrow = async () => {};
        var generator = function* () {};
        const asyncGenerator = async function* () {};
        expectLengthAndName(arrow, 0, "arrow");
        expectLengthAndName(asyncArrow, 0, "asyncArrow");
        expectLengthAndName(generator, 0, "generator");
        expectLengthAndName(asyncGenerator, 0, "asyncGenerator");
        expect(Reflect.ownKeys(arrow)).toEqual(["length", "name"]);
        expect(Reflect.ownKeys(generator)).toEqual(["length", "name", "prototype"]);

        const { defaulted = x => x } = {};
        expectLengthAndName(defaulted, 1, "defaulted");

        expectLengthAndName(() => {}, 0, "");
    });

    test("dynamic functions", () => {
        const AsyncFunction = AsyncFunctionPrototype.constructor;
        const GeneratorFunction = GeneratorFunctionPrototype.constructor;
        const AsyncGeneratorFunction = AsyncGeneratorFunctionPrototype.constructor;

        const asyncFunction = new AsyncFunction("a", "");
        expect(Reflect.ownKeys(asyncFunction)).toEqual(["length", "name"]);
        expectLengthAndName(asyncFunction, 1, "anonymous");

        const generator = new GeneratorFunction("a", "b", "");
        expect(Reflect.ownKeys(generator)).toEqual(["length", "name", "prototype"]);
        expectLengthAndName(generator, 2, "anonymous");
        expectPrototypeProperty(generator, GeneratorFunctionPrototype.prototype);

        const asyncGenerator = new AsyncGeneratorFunction("");
        expect(Reflect.ownKeys(asyncGenerator)).toEqual(["length", "name", "prototype"]);
        expectPrototypeProperty(asyncGenerator, AsyncGeneratorFunctionPrototype.prototype);

        class MyGeneratorFunction extends GeneratorFunction {}
        const subclassed = new MyGeneratorFunction("");
        expect(Object.getPrototypeOf(subclassed)).toBe(MyGeneratorFunction.prototype);
        expect(Reflect.ownKeys(subclassed)).toEqual(["length", "name", "prototype"]);
    });

    test("bound functions", () => {
        const bound = (async (a, b) => {}).bind(null, 1);
        expect(Reflect.ownKeys(bound)).toEqual(["length", "name"]);
        expectLengthAndName(bound, 1, "bound ");
    });

    test("toString is unaffected", () => {
        const arrow = (a, b) => a;
        async function* asyncGenerator() {}
        expect(arrow.toString()).toBe("(a, b) => a");
        expect(asyncGenerator.toString()).toBe("async function* asyncGenerator() {}");
    });
});

describe("redefining and deleting length and name", () => {
    const makers = {
        arrow: () => (a, b) => {},
        "async arrow": () => async (a, b) => {},
        "async function": () => async function (a, b) {},
        generator: () => function* (a, b) {},
        "async generator": () => async function* (a, b) {},
    };

    for (const [label, make] of Object.entries(makers)) {
        test(label, () => {
            const fn = make();
            const hasPrototype = fn.hasOwnProperty("prototype");
            const tail = hasPrototype ? ["prototype"] : [];

            Object.defineProperty(fn, "name", { value: "renamed" });
            expect(fn.name).toBe("renamed");
            expect(Reflect.ownKeys(fn)).toEqual(["length", "name", ...tail]);
            expect(Object.getOwnPropertyDescriptor(fn, "name").writable).toBeFalse();

            Object.defineProperty(fn, "length", { value: 42, writable: true, enumerable: true });
            expect(Object.getOwnPropertyDescriptor(fn, "length")).toEqual({
                value: 42,
                writable: true,
                enumerable: true,
                configurable: true,
            });
            fn.length = 7;
            expect(fn.length).toBe(7);
            expect(Object.keys(fn)).toEqual(["length"]);

            expect(delete fn.name).toBeTrue();
            expect(fn.hasOwnProperty("name")).toBeFalse();
            expect(Reflect.ownKeys(fn)).toEqual(["length", ...tail]);
            expect(fn.name).toBe("");

            fn.name = "assigned";
            expect(fn.hasOwnProperty("name")).toBeFalse();
            Object.defineProperty(fn, "name", { value: "readded", configurable: true });
            expect(Reflect.ownKeys(fn)).toEqual(["length", ...tail, "name"]);

            expect(delete fn.length).toBeTrue();
            expect(Reflect.ownKeys(fn)).toEqual([...tail, "name"]);

            if (hasPrototype) {
                expect(delete fn.prototype).toBeFalse();
                fn.prototype = null;
                expect(fn.prototype).toBeNull();
            }

            // Other functions of the same kind stay untouched.
            const fresh = make();
            expect(Reflect.ownKeys(fresh)).toEqual(["length", "name", ...tail]);
            expectLengthAndName(fresh, 2, "");
        });
    }

    test("non-configurable after freeze", () => {
        const fn = async () => {};
        Object.freeze(fn);
        expect(Object.getOwnPropertyDescriptor(fn, "name").configurable).toBeFalse();
        expect(Object.getOwnPropertyDescriptor(fn, "length").configurable).toBeFalse();
        expect(delete fn.name).toBeFalse();
        const fresh = async () => {};
        expect(Object.getOwnPropertyDescriptor(fresh, "name").configurable).toBeTrue();
    });

    test("adding properties does not leak to other functions", () => {
        const first = () => {};
        const second = () => {};
        first.extra = 1;
        expect(Reflect.ownKeys(first)).toEqual(["length", "name", "extra"]);
        expect(Reflect.ownKeys(second)).toEqual(["length", "name"]);
    });
});
