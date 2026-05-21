test("class properties", () => {
    class A {}
    expect(A.name).toBe("A");
    expect(A).toHaveLength(0);
});

test("class constructor prototype property descriptor", () => {
    class A {}

    const descriptor = Object.getOwnPropertyDescriptor(A, "prototype");
    expect(descriptor.value).toBe(A.prototype);
    expect(descriptor.writable).toBeFalse();
    expect(descriptor.enumerable).toBeFalse();
    expect(descriptor.configurable).toBeFalse();

    const constructorDescriptor = Object.getOwnPropertyDescriptor(A.prototype, "constructor");
    expect(constructorDescriptor.value).toBe(A);
    expect(constructorDescriptor.writable).toBeTrue();
    expect(constructorDescriptor.enumerable).toBeFalse();
    expect(constructorDescriptor.configurable).toBeTrue();
});

test("class heritage and computed property names are strict mode code", () => {
    expect(() => {
        class A {
            [(Object.preventExtensions({}).prop = 1)]() {}
        }
    }).toThrow(TypeError);

    expect(() => {
        const A = class {
            [(Object.preventExtensions({}).prop = 1)]() {}
        };
    }).toThrow(TypeError);

    expect(() => {
        class A {
            [(Object.preventExtensions({}).prop = 1)];
        }
    }).toThrow(TypeError);

    expect(() => {
        class A extends (Object.preventExtensions({}).prop = 1) {}
    }).toThrow(TypeError);

    expect(() => {
        const A = class extends (Object.preventExtensions({}).prop = 1) {};
    }).toThrow(TypeError);
});
