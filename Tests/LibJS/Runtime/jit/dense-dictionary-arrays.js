function filledTexture() {
    const values = new Array(16 * 16 * 3 * 16);
    for (let i = 768; i < values.length; ++i) values[i] = i;
    return values;
}

test("mostly filled dictionary arrays preserve holes and promote to holey storage", () => {
    const values = filledTexture();
    expect(values.length).toBe(12288);
    expect(0 in values).toBeFalse();
    expect(767 in values).toBeFalse();
    expect(values[767]).toBeUndefined();
    expect(values[768]).toBe(768);
    expect(values[12287]).toBe(12287);
    expect(Object.keys(values).length).toBe(11520);
    if (jit.enabled) {
        const read = new Function("values", "index", "return values[index];");
        jit.prepare(read);
        read(values, 1000);
        expect(jit.feedback(read).includes("elements=Holey")).toBeTrue();
        jit.compile(read);
        expect(read(values, 12287)).toBe(12287);
        expect(read(values, 0)).toBeUndefined();
        const prototype = Object.create(Array.prototype);
        prototype[0] = 42;
        Object.setPrototypeOf(values, prototype);
        expect(read(values, 0)).toBe(42);
        delete values[1000];
        expect(read(values, 1000)).toBeUndefined();
    }
});

test("dictionary promotion retains indexed attributes and accessors", () => {
    const values = new Array(1024);
    let calls = 0;
    Object.defineProperty(values, 768, {
        get() {
            ++calls;
            return 7;
        },
        enumerable: true,
        configurable: true,
    });
    Object.defineProperty(values, 769, { value: 9, writable: false, enumerable: false, configurable: false });
    for (let i = 770; i < 1024; ++i) values[i] = i;
    for (let i = 0; i < 768; ++i) values[i] = i;
    expect(values[768]).toBe(7);
    expect(calls).toBe(1);
    expect(Object.getOwnPropertyDescriptor(values, "769")).toEqual({
        value: 9,
        writable: false,
        enumerable: false,
        configurable: false,
    });
    values[769] = 100;
    expect(values[769]).toBe(9);
    expect(Object.keys(values)).not.toContain("769");
});

test("dense promotion preserves shrinking, growing, and sparse lengths", () => {
    const values = filledTexture();
    values.length = 800;
    expect(values[799]).toBe(799);
    expect(values[800]).toBeUndefined();
    expect(0 in values).toBeFalse();
    values.length = 12288;
    expect(800 in values).toBeFalse();
    values[1000] = 42;
    expect(values[1000]).toBe(42);
    expect(999 in values).toBeFalse();
    const sparse = new Array(0xffffffff);
    sparse[1000] = 7;
    expect(sparse.length).toBe(0xffffffff);
    expect(sparse[1000]).toBe(7);
    expect(999 in sparse).toBeFalse();
    expect(Object.keys(sparse)).toEqual(["1000"]);
});

test("dictionary arrays that start above the density threshold keep their elements", () => {
    const values = [];
    for (let i = 0; i < 1024; ++i) values[i] = i;
    // NB: A read-only element makes the elements a dictionary, which is
    //     already denser than the promotion threshold.
    Object.defineProperty(values, 900, { value: 900, writable: false, enumerable: true, configurable: true });
    delete values[900];
    delete values[901];
    values[1100] = 1100;
    expect(values.length).toBe(1101);
    expect(900 in values).toBeFalse();
    expect(901 in values).toBeFalse();
    expect(1024 in values).toBeFalse();
    expect(values[899]).toBe(899);
    expect(values[1023]).toBe(1023);
    expect(values[1100]).toBe(1100);
    expect(Object.keys(values).length).toBe(1023);
    values[900] = 900;
    values[901] = 901;
    for (let i = 1024; i < 1100; ++i) values[i] = i;
    expect(Object.keys(values).length).toBe(1101);
    expect(values.every((value, index) => value === index)).toBeTrue();
    const read = new Function("values", "index", "return values[index];");
    jit.prepare(read);
    for (let i = 0; i < 10; ++i) read(values, i);
    jit.compile(read);
    expect(read(values, 901)).toBe(901);
    expect(read(values, 1100)).toBe(1100);
    expect(read(values, 1101)).toBeUndefined();
});

function elementsKindSeen(values, index) {
    const read = new Function("values", "index", "return values[index];");
    jit.prepare(read);
    read(values, index);
    return jit.feedback(read);
}

test("sparse stores past elements that stay dense enough keep them dense", () => {
    const packed = [];
    for (let i = 0; i < 1024; ++i) packed[i] = i;
    packed[1300] = 1300;
    const holey = [];
    for (let i = 0; i < 1024; ++i) if (i % 16) holey[i] = i;
    holey[1250] = 1250;
    for (const [values, last, count] of [
        [packed, 1300, 1025],
        [holey, 1250, 961],
    ]) {
        expect(values.length).toBe(last + 1);
        expect(1024 in values).toBeFalse();
        expect(values[last - 1]).toBeUndefined();
        expect(values[last]).toBe(last);
        expect(Object.keys(values).length).toBe(count);
        values[1024] = 1024;
        expect(values[1024]).toBe(1024);
        if (jit.enabled) expect(elementsKindSeen(values, 1000).includes("elements=Holey")).toBeTrue();
    }

    // Stores that would leave the elements sparse make them a dictionary.
    const sparse = [];
    for (let i = 0; i < 1024; ++i) sparse[i] = i;
    sparse[1400] = 1400;
    expect(sparse[1400]).toBe(1400);
    expect(Object.keys(sparse).length).toBe(1025);
    if (jit.enabled) expect(elementsKindSeen(sparse, 1000).includes("elements=Other")).toBeTrue();
});

test("repeated sparse stores past holey elements that stay dense keep them dense", () => {
    // NB: Each store lands past the elements' storage, and the gaps fill in
    //     but for one hole, so the elements stay dense. Counting the holes at
    //     every such store would take quadratic time.
    const values = [];
    for (let i = 0; i < 1024; ++i) if (i % 16) values[i] = i;
    for (let k = 0; k < 10000; ++k) {
        const start = values.length;
        values[start + 201] = start + 201;
        for (let i = start + 1; i < start + 201; ++i) values[i] = i;
    }
    expect(values.length).toBe(1024 + 10000 * 202);
    expect(0 in values).toBeFalse();
    expect(1024 in values).toBeFalse();
    expect(values[1025]).toBe(1025);
    expect(values[values.length - 1]).toBe(values.length - 1);
    if (jit.enabled) expect(elementsKindSeen(values, 1025).includes("elements=Holey")).toBeTrue();
});
