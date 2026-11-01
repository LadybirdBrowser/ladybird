// Private elements live in the storage of the objects they are added to, as properties nothing outside the engine sees.

class Stamper extends class {
    constructor(object) {
        return object;
    }
} {
    #field = 1;
    #method() {
        return "method";
    }
    get #accessor() {
        return this.#field;
    }
    set #accessor(value) {
        this.#field = value;
    }

    static has(object) {
        return #field in object;
    }

    static field(object) {
        return object.#field;
    }

    static setField(object, value) {
        object.#field = value;
    }

    static callMethod(object) {
        return object.#method();
    }

    static setMethod(object) {
        object.#method = null;
    }

    static setAccessor(object, value) {
        object.#accessor = value;
        return object.#accessor;
    }
}

test("private elements are not properties", () => {
    const object = new Stamper({ visible: 1 });
    expect(Stamper.has(object)).toBeTrue();
    expect(Reflect.ownKeys(object)).toEqual(["visible"]);
    expect({ ...object }).toEqual({ visible: 1 });
    expect(Object.assign({}, object)).toEqual({ visible: 1 });
    expect(JSON.stringify(object)).toBe('{"visible":1}');
});

test("fields stay writable on frozen objects", () => {
    const object = new Stamper({});
    Object.freeze(object);
    expect(Object.isFrozen(object)).toBeTrue();
    Stamper.setField(object, 2);
    expect(Stamper.field(object)).toBe(2);
    expect(Stamper.setAccessor(object, 3)).toBe(3);
});

test("methods are read-only", () => {
    const object = new Stamper({});
    expect(Stamper.callMethod(object)).toBe("method");
    expect(() => Stamper.setMethod(object)).toThrowWithMessage(TypeError, "Cannot set private method");
});

test("proxies keep private elements without calling traps", () => {
    const proxy = new Proxy(
        {},
        new Proxy(
            {},
            {
                get() {
                    throw new Error("trap called");
                },
            }
        )
    );
    new Stamper(proxy);
    Stamper.setField(proxy, 2);
    expect(Stamper.field(proxy)).toBe(2);
});

test("private elements survive dictionary objects losing properties", () => {
    const object = {};
    for (let i = 0; i < 100; ++i) object[`property${i}`] = i;
    new Stamper(object);
    for (let i = 0; i < 100; ++i) delete object[`property${i}`];
    expect(Stamper.field(object)).toBe(1);
    expect(Stamper.setAccessor(object, 4)).toBe(4);
    expect(Reflect.ownKeys(object)).toEqual([]);
});

test("each evaluation of a class has private names of its own", () => {
    const makeClass = () =>
        class {
            #field;
            static has(object) {
                return #field in object;
            }
        };
    const A = makeClass();
    const B = makeClass();
    const a = new A();
    expect(A.has(a)).toBeTrue();
    expect(B.has(a)).toBeFalse();
});
