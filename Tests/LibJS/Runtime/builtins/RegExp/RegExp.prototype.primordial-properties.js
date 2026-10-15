const originalExec = RegExp.prototype.exec;

function runFastPaths() {
    for (let i = 0; i < 10; ++i) {
        expect(/b/.test("abc")).toBeTrue();
        expect("abc".replace(/b/g, "x")).toBe("axc");
        expect("a,b".split(/,/)).toEqual(["a", "b"]);
    }
}

function withProperty(object, key, descriptor, callback) {
    const original = Object.getOwnPropertyDescriptor(object, key);
    Object.defineProperty(object, key, descriptor);
    try {
        callback();
    } finally {
        Object.defineProperty(object, key, original);
    }
}

test("a replaced exec is called after the fast paths ran", () => {
    runFastPaths();
    let calls = 0;
    RegExp.prototype.exec = function (string) {
        ++calls;
        return originalExec.call(this, string);
    };
    try {
        expect(/b/.test("abc")).toBeTrue();
        expect("abc".replace(/b/, "x")).toBe("axc");
        expect(calls).toBe(2);
    } finally {
        RegExp.prototype.exec = originalExec;
    }
});

test("a replaced flag getter is called after the fast paths ran", () => {
    runFastPaths();
    let calls = 0;
    withProperty(
        RegExp.prototype,
        "hasIndices",
        {
            get() {
                ++calls;
                return false;
            },
            configurable: true,
        },
        () => {
            expect("abab".replace(/b/, "x")).toBe("axab");
            expect(calls).toBe(1);
        }
    );
});

test("a replaced constructor is used by split after the fast paths ran", () => {
    runFastPaths();
    let calls = 0;
    function Splitter(pattern, flags) {
        ++calls;
        return new RegExp(pattern, flags);
    }
    Splitter[Symbol.species] = Splitter;
    withProperty(RegExp.prototype, "constructor", { value: Splitter, configurable: true, writable: true }, () => {
        expect("a,b".split(/,/)).toEqual(["a", "b"]);
        expect(calls).toBe(1);
    });
});

test("a replaced @@species getter is used by split after the fast paths ran", () => {
    runFastPaths();
    let calls = 0;
    withProperty(
        RegExp,
        Symbol.species,
        {
            get() {
                ++calls;
                return RegExp;
            },
            configurable: true,
        },
        () => {
            expect("a,b".split(/,/)).toEqual(["a", "b"]);
            expect(calls).toBe(1);
        }
    );
});

test("an accessor @@match is read by split after the fast paths ran", () => {
    runFastPaths();
    let calls = 0;
    const originalMatch = RegExp.prototype[Symbol.match];
    withProperty(
        RegExp.prototype,
        Symbol.match,
        {
            get() {
                ++calls;
                return originalMatch;
            },
            configurable: true,
        },
        () => {
            expect("a,b".split(/,/)).toEqual(["a", "b"]);
            expect(calls).toBeGreaterThan(0);
        }
    );
});

test("a replaced exec is called after unrelated properties changed", () => {
    runFastPaths();
    for (const object of [RegExp.prototype, RegExp, Object.prototype]) {
        object.unrelated = 1;
        runFastPaths();
        delete object.unrelated;
        runFastPaths();
    }
    let calls = 0;
    RegExp.prototype.exec = function (string) {
        ++calls;
        return originalExec.call(this, string);
    };
    try {
        expect(/b/.test("abc")).toBeTrue();
        expect(calls).toBe(1);
    } finally {
        RegExp.prototype.exec = originalExec;
    }
});
