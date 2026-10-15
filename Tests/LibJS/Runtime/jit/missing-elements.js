function compileWith(f, ...args) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...args);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

function at(array, index) {
    return array[index];
}

function warmUp() {
    // NB: The feedback sees elements, holes and indices past the end of packed and holey arrays.
    compileWith(array => [at(array, 0), at(array, 1), at(array, 5), at([1, , 3], 1)], [1, 2]);
    jit.prepare(at);
    for (let i = 0; i < 10; ++i) {
        at([1, 2], 0);
        at([1, 2], 5);
        at([1, , 3], 1);
    }
    expect(jit.compile(at)).toBe(jit.enabled);
}

test("holes and indices past the end read undefined", () => {
    warmUp();
    expect(at([1, 2], 0)).toBe(1);
    expect(at([1, 2], 2)).toBeUndefined();
    expect(at([1, 2], 1000)).toBeUndefined();
    expect(at([1, , 3], 1)).toBeUndefined();
    expect(at([], 0)).toBeUndefined();
    const sparse = [1, 2, 3];
    sparse.length = 100;
    expect(at(sparse, 50)).toBeUndefined();
    expect(at(sparse, 2)).toBe(3);
});

test("negative indices read named properties", () => {
    warmUp();
    const array = [1, 2];
    array["-1"] = "named";
    expect(at(array, -1)).toBe("named");
    expect(at([1, 2], -1)).toBeUndefined();
});

test("holes and indices past the end read indexed properties of Array.prototype", () => {
    warmUp();
    Array.prototype[5] = "array prototype";
    Array.prototype[1] = "array prototype hole";
    try {
        expect(at([1, 2], 5)).toBe("array prototype");
        expect(at([1, , 3], 1)).toBe("array prototype hole");
    } finally {
        delete Array.prototype[5];
        delete Array.prototype[1];
    }
    expect(at([1, 2], 5)).toBeUndefined();
});

test("holes and indices past the end read indexed properties of Object.prototype", () => {
    warmUp();
    Object.prototype[5] = "object prototype";
    try {
        expect(at([1, 2], 5)).toBe("object prototype");
    } finally {
        delete Object.prototype[5];
    }
    expect(at([1, 2], 5)).toBeUndefined();
});

test("holes and indices past the end call getters of prototypes", () => {
    warmUp();
    let calls = 0;
    Object.defineProperty(Array.prototype, 7, {
        get() {
            ++calls;
            return "getter";
        },
        configurable: true,
    });
    try {
        expect(at([1, 2], 7)).toBe("getter");
        expect(calls).toBe(1);
    } finally {
        delete Array.prototype[7];
    }
});

test("holes and indices past the end read other prototypes", () => {
    warmUp();
    const array = [1, 2];
    Object.setPrototypeOf(array, { 5: "own prototype" });
    expect(at(array, 5)).toBe("own prototype");

    const proxied = [1, , 3];
    Object.setPrototypeOf(proxied, new Proxy([], { get: () => "proxy" }));
    expect(at(proxied, 1)).toBe("proxy");
    expect(at(proxied, 10)).toBe("proxy");
});
