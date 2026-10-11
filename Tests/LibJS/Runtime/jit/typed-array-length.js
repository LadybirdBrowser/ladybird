function compileLength() {
    const lengthOf = new Function("array", "return array.length;");
    jit.prepare(lengthOf);
    for (let i = 0; i < 10; ++i) {
        lengthOf(new Uint8Array(3));
        lengthOf(new Float64Array(4));
    }
    expect(jit.compile(lengthOf)).toBe(jit.enabled);
    return lengthOf;
}

test("typed arrays of several kinds read their length", () => {
    const lengthOf = compileLength();
    expect(lengthOf(new Uint8Array(7))).toBe(7);
    expect(lengthOf(new Float64Array(2))).toBe(2);
    expect(lengthOf(new Uint8Array(new ArrayBuffer(16), 4, 5))).toBe(5);
    expect(jit.exitCount(lengthOf)).toBe(0);
});

test("typed arrays without a cached length take the slow path", () => {
    const lengthOf = compileLength();
    expect(lengthOf(new Uint8Array(0))).toBe(0);

    const detached = new Uint8Array(new ArrayBuffer(8));
    detached.buffer.transfer();
    expect(lengthOf(detached)).toBe(0);

    const buffer = new ArrayBuffer(4, { maxByteLength: 16 });
    const tracking = new Uint8Array(buffer);
    expect(lengthOf(tracking)).toBe(4);
    buffer.resize(12);
    expect(lengthOf(tracking)).toBe(12);
    const fixed = new Uint8Array(buffer, 8, 4);
    buffer.resize(10);
    expect(lengthOf(fixed)).toBe(0);
    expect(jit.exitCount(lengthOf)).toBe(0);
});

test("objects that inherit from a typed array prototype call the getter", () => {
    const lengthOf = compileLength();
    expect(() => lengthOf(Object.create(Uint8Array.prototype))).toThrowWithMessage(
        TypeError,
        "Not an object of type TypedArray"
    );
    expect(lengthOf(new Uint8Array(3))).toBe(3);
});

test("own and redefined length properties are read", () => {
    const lengthOf = compileLength();
    const array = new Uint8Array(3);
    Object.defineProperty(array, "length", { value: 42 });
    expect(lengthOf(array)).toBe(42);

    const descriptor = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "length");
    Object.defineProperty(Object.getPrototypeOf(Uint8Array.prototype), "length", {
        get() {
            return "redefined";
        },
        configurable: true,
    });
    try {
        expect(lengthOf(new Uint8Array(3))).toBe("redefined");
    } finally {
        Object.defineProperty(Object.getPrototypeOf(Uint8Array.prototype), "length", descriptor);
    }
    expect(lengthOf(new Uint8Array(3))).toBe(3);
});

test("length properties of one kind's prototype are read", () => {
    const lengthOf = compileLength();
    Object.defineProperty(Float64Array.prototype, "length", { value: "shadowed", configurable: true });
    try {
        expect(lengthOf(new Float64Array(4))).toBe("shadowed");
        expect(lengthOf(new Uint8Array(3))).toBe(3);
    } finally {
        delete Float64Array.prototype.length;
    }
    expect(lengthOf(new Float64Array(4))).toBe(4);
});
