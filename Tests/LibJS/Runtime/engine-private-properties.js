test("engine-private properties are not exposed as ECMAScript properties", () => {
    const publicSymbol = Symbol("public");
    const object = { visible: 1, [publicSymbol]: 2 };
    addEnginePrivateProperty(object, 3);

    expect(Reflect.ownKeys(object)).toEqual(["visible", publicSymbol]);
    expect(Object.getOwnPropertyNames(object)).toEqual(["visible"]);
    expect(Object.getOwnPropertySymbols(object)).toEqual([publicSymbol]);
    expect(Object.getOwnPropertyDescriptors(object)).toEqual({
        visible: {
            configurable: true,
            enumerable: true,
            value: 1,
            writable: true,
        },
        [publicSymbol]: {
            configurable: true,
            enumerable: true,
            value: 2,
            writable: true,
        },
    });

    const enumeratedKeys = [];
    for (const key in object) enumeratedKeys.push(key);
    expect(enumeratedKeys).toEqual(["visible"]);
    expect({ ...object }).toEqual({ visible: 1, [publicSymbol]: 2 });
    expect(Object.assign({}, object)).toEqual({ visible: 1, [publicSymbol]: 2 });
});

test("engine-private properties do not participate in proxy ownKeys invariants", () => {
    const target = {};
    addEnginePrivateProperty(target, 1);
    Object.preventExtensions(target);

    let ownKeysCallCount = 0;
    const proxy = new Proxy(target, {
        ownKeys() {
            ++ownKeysCallCount;
            return [];
        },
    });

    expect(Reflect.ownKeys(proxy)).toEqual([]);
    expect(ownKeysCallCount).toBe(1);
});

test("engine-private properties do not affect integrity levels", () => {
    const object = {};
    addEnginePrivateProperty(object, 1);
    Object.freeze(object);

    expect(Object.isFrozen(object)).toBeTrue();
    addEnginePrivateProperty(object, 2);
    expect(Object.isFrozen(object)).toBeTrue();
    expect(Reflect.ownKeys(object)).toEqual([]);
});

test("engine-private properties are hidden on exotic objects", () => {
    const string = new String("x");
    const typedArray = new Uint8Array(1);
    addEnginePrivateProperty(string, 1);
    addEnginePrivateProperty(typedArray, 1);

    expect(Reflect.ownKeys(string)).toEqual(["0", "length"]);
    expect(Reflect.ownKeys(typedArray)).toEqual(["0"]);
});
