describe("correct behavior", () => {
    test("basic functionality", () => {
        let n = 0;
        expect(++n).toBe(1);
        expect(n).toBe(1);

        n = 0;
        expect(n++).toBe(0);
        expect(n).toBe(1);

        n = 0;
        expect(--n).toBe(-1);
        expect(n).toBe(-1);

        n = 0;
        expect(n--).toBe(0);
        expect(n).toBe(-1);

        let a = [];
        expect(a++).toBe(0);
        expect(a).toBe(1);

        let b = true;
        expect(b--).toBe(1);
        expect(b).toBe(0);
    });

    test("updates that produce NaN", () => {
        let s = "foo";
        expect(++s).toBeNaN();
        expect(s).toBeNaN();

        s = "foo";
        expect(s++).toBeNaN();
        expect(s).toBeNaN();

        s = "foo";
        expect(--s).toBeNaN();
        expect(s).toBeNaN();

        s = "foo";
        expect(s--).toBeNaN();
        expect(s).toBeNaN();
    });
});

describe("errors", () => {
    test("update expression throws reference error", () => {
        expect(() => {
            ++x;
        }).toThrowWithMessage(ReferenceError, "'x' is not defined");
    });
});
