test("length is 1", () => {
    expect(Array.from).toHaveLength(1);
});

describe("normal behavior", () => {
    test("empty array, no mapFn", () => {
        const a = Array.from([]);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(0);
    });

    test("empty array, with mapFn, no thisArg", () => {
        const a = Array.from([], n => n);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(0);
    });

    test("empty array, with mapFn, with thisArg", () => {
        const a = Array.from(
            [],
            function (n) {
                return n + this.value;
            },
            { value: 100 }
        );
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(0);
    });

    test("empty string, no mapFn", () => {
        const a = Array.from("");
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(0);
    });

    test("empty string, with mapFn, no thisArg", () => {
        const a = Array.from("", n => n);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(0);
    });

    test("empty string, with mapFn, with thisArg", () => {
        const a = Array.from(
            "",
            function (n) {
                return n + this.value;
            },
            { value: 100 }
        );
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(0);
    });

    test("non-empty array, no mapFn", () => {
        const a = Array.from([5, 8, 1]);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(5);
        expect(a[1]).toBe(8);
        expect(a[2]).toBe(1);
    });

    test("non-empty array, with mapFn, no thisArg", () => {
        const a = Array.from([5, 8, 1], (n, i) => n - i);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(5);
        expect(a[1]).toBe(7);
        expect(a[2]).toBe(-1);
    });

    test("non-empty array, with mapFn, with thisArg", () => {
        const a = Array.from(
            [5, 8, 1],
            function (n, i) {
                return n - i + this.value;
            },
            { value: 100 }
        );
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(105);
        expect(a[1]).toBe(107);
        expect(a[2]).toBe(99);
    });

    test("non-empty string, no mapFn", () => {
        const a = Array.from("what");
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(4);
        expect(a[0]).toBe("w");
        expect(a[1]).toBe("h");
        expect(a[2]).toBe("a");
        expect(a[3]).toBe("t");
    });

    test("non-empty string, with mapFn, no thisArg", () => {
        const a = Array.from("what", (n, i) => n + n + i);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(4);
        expect(a[0]).toBe("ww0");
        expect(a[1]).toBe("hh1");
        expect(a[2]).toBe("aa2");
        expect(a[3]).toBe("tt3");
    });

    test("non-empty string, with mapFn, with thisArg", () => {
        const a = Array.from(
            "what",
            function (n, i) {
                return n + i + this.value;
            },
            { value: "a" }
        );
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(4);
        expect(a[0]).toBe("w0a");
        expect(a[1]).toBe("h1a");
        expect(a[2]).toBe("a2a");
        expect(a[3]).toBe("t3a");
    });

    test("shallow array copy, no mapFn", () => {
        const a = [1, 2, 3];
        const b = Array.from([a]);
        expect(b instanceof Array).toBeTrue();
        expect(b).toHaveLength(1);
        b[0][0] = 4;
        expect(a[0]).toBe(4);
    });

    test("shallow array copy, with mapFn, no thisArg", () => {
        const a = [1, 2, 3];
        const b = Array.from([a], n => n.map(n => n + 2));
        expect(b instanceof Array).toBeTrue();
        expect(b).toHaveLength(1);
        b[0][0] = 10;
        expect(a[0]).toBe(1);
        expect(b[0][0]).toBe(10);
        expect(b[0][1]).toBe(4);
        expect(b[0][2]).toBe(5);
    });

    test("shallow array copy, with mapFn, with thisArg", () => {
        const a = [1, 2, 3];
        const b = Array.from(
            [a],
            function (n, i) {
                return n.map(n => n + 2 + i + this.value);
            },
            { value: 100 }
        );
        expect(b instanceof Array).toBeTrue();
        expect(b).toHaveLength(1);
        b[0][0] = 10;
        expect(a[0]).toBe(1);
        expect(b[0][0]).toBe(10);
        expect(b[0][1]).toBe(104);
        expect(b[0][2]).toBe(105);
    });

    const rangeIterator = function (begin, end) {
        return {
            [Symbol.iterator]() {
                let value = begin - 1;
                return {
                    next() {
                        if (value < end) {
                            value += 1;
                        }
                        return { value: value, done: value >= end };
                    },
                };
            },
        };
    };

    test("from iterator, no mapFn", () => {
        const a = Array.from(rangeIterator(8, 10));
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(2);
        expect(a[0]).toBe(8);
        expect(a[1]).toBe(9);
    });

    test("from iterator, with mapFn, no thisArg", () => {
        const a = Array.from(rangeIterator(8, 10), n => --n);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(2);
        expect(a[0]).toBe(7);
        expect(a[1]).toBe(8);
    });

    test("from iterator, with mapFn, with thisArg", () => {
        const a = Array.from(
            rangeIterator(8, 10),
            function (n, i) {
                return n + i + this.value;
            },
            { value: 100 }
        );
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(2);
        expect(a[0]).toBe(108);
        expect(a[1]).toBe(110);
    });
});
