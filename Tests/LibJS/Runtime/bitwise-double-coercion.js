describe("bitwise operations on double values (ToInt32 coercion)", () => {
    test("bitwise OR with zero truncates doubles", () => {
        expect(3.7 | 0).toBe(3);
        expect(3.2 | 0).toBe(3);
        expect(-3.7 | 0).toBe(-3);
        expect(-3.2 | 0).toBe(-3);
        expect(0.9 | 0).toBe(0);
        expect(-0.9 | 0).toBe(0);
    });

    test("bitwise AND on doubles", () => {
        expect(255.7 & 0x0f).toBe(15);
        expect(-1.5 & 0xff).toBe(255);
        expect(3.14 & 7).toBe(3);
    });

    test("bitwise XOR on doubles", () => {
        expect(10.5 ^ 3).toBe(9);
        expect(-10.5 ^ -1).toBe(9);
    });

    test("left shift on doubles", () => {
        expect(1.5 << 4).toBe(16);
        expect(-1.5 << 4).toBe(-16);
        expect(3.9 << 1).toBe(6);
    });

    test("right shift on doubles", () => {
        expect(100.7 >> 2).toBe(25);
        expect(-100.7 >> 2).toBe(-25);
    });

    test("unsigned right shift on doubles", () => {
        expect(-1.5 >>> 0).toBe(4294967295);
        expect(3.7 >>> 0).toBe(3);
    });

    test("NaN coerces to 0 in bitwise ops", () => {
        expect(NaN | 0).toBe(0);
        expect(NaN & 0xff).toBe(0);
        expect(NaN ^ 5).toBe(5);
        expect(NaN << 3).toBe(0);
        expect(NaN >> 3).toBe(0);
        expect(NaN >>> 3).toBe(0);
    });

    test("Infinity coerces to 0 in bitwise ops", () => {
        expect(Infinity | 0).toBe(0);
        expect(-Infinity | 0).toBe(0);
        expect(Infinity & 0xff).toBe(0);
        expect(-Infinity ^ 5).toBe(5);
    });

    test("negative zero coerces to 0 in bitwise ops", () => {
        expect(-0 | 0).toBe(0);
        expect(-0 & 0xff).toBe(0);
        expect(-0 ^ 5).toBe(5);
    });

    test("large doubles wrap via ToInt32", () => {
        // 2^32 wraps to 0
        expect(4294967296 | 0).toBe(0);
        // 2^31 wraps to -2147483648
        expect(2147483648 | 0).toBe(-2147483648);
        // 2^31 - 1 stays as is
        expect(2147483647 | 0).toBe(2147483647);
    });

    test("both operands are doubles", () => {
        expect(3.5 & 7.9).toBe(3);
        expect(10.1 | 5.9).toBe(15);
        expect(15.7 ^ 9.2).toBe(6);
    });

    test("mixed int32 and double operands", () => {
        let x = 3.5;
        let y = 7;
        expect(x & y).toBe(3);
        expect(x | y).toBe(7);
        expect(x ^ y).toBe(4);
    });

    test("double bitwise in loop (common optimization pattern)", () => {
        let sum = 0;
        for (let i = 0; i < 10; i++) {
            sum += (i * 2.5) | 0;
        }
        // 0+0+2+5+7+10+12+15+17+20 = 88 (wait let me recalculate)
        // i=0: 0|0=0, i=1: 2.5|0=2, i=2: 5|0=5, i=3: 7.5|0=7
        // i=4: 10|0=10, i=5: 12.5|0=12, i=6: 15|0=15, i=7: 17.5|0=17
        // i=8: 20|0=20, i=9: 22.5|0=22
        // sum = 0+2+5+7+10+12+15+17+20+22 = 110
        expect(sum).toBe(110);
    });

    test("bitwise NOT on double (~)", () => {
        expect(~3.7).toBe(-4);
        expect(~-3.7).toBe(2);
        expect(~NaN).toBe(-1);
        expect(~Infinity).toBe(-1);
    });
});

describe("int32 re-boxing after double arithmetic", () => {
    test("double addition producing whole number re-boxes as int32", () => {
        // 1.5 + 1.5 = 3.0, which should be re-boxed as Int32(3)
        let x = 1.5 + 1.5;
        // Verify it behaves as an integer
        expect(x).toBe(3);
        expect(x === 3).toBeTrue();
    });

    test("double subtraction producing whole number", () => {
        let x = 5.5 - 2.5;
        expect(x).toBe(3);
    });

    test("double multiplication producing whole number", () => {
        let x = 2.5 * 4;
        expect(x).toBe(10);
    });

    test("double division producing whole number", () => {
        let x = 10.0 / 2.0;
        expect(x).toBe(5);
    });

    test("negative zero stays as double (not re-boxed as int32)", () => {
        let x = 0.0 * -1;
        expect(Object.is(x, -0)).toBeTrue();
    });

    test("Math.floor producing whole number", () => {
        expect(Math.floor(3.7)).toBe(3);
        expect(Math.floor(-3.7)).toBe(-4);
        expect(Math.floor(3.0)).toBe(3);
    });

    test("Math.ceil producing whole number", () => {
        expect(Math.ceil(3.2)).toBe(4);
        expect(Math.ceil(-3.2)).toBe(-3);
        expect(Math.ceil(3.0)).toBe(3);
    });

    test("results at INT32 boundaries", () => {
        // INT32_MAX = 2147483647
        let x = 2147483646.0 + 1.0;
        expect(x).toBe(2147483647);
        // INT32_MAX + 1 should stay as double
        let y = 2147483647.0 + 1.0;
        expect(y).toBe(2147483648);
        // INT32_MIN = -2147483648
        let z = -2147483647.0 - 1.0;
        expect(z).toBe(-2147483648);
    });

    test("chained arithmetic with re-boxing", () => {
        // Simulate a pattern from raycasting: integer arithmetic via doubles
        let dist = 10.0;
        let ddist = 255 - (((dist / 32) * 255) | 0);
        expect(ddist).toBe(176);
    });
});
