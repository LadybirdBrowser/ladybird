test("numeric properties", () => {
    const i32Max = 2 ** 31 - 1;
    const u32Max = 2 ** 32 - 1;
    const o = {
        [-1]: "foo",
        0: "foo",
        1: "foo",
        [i32Max - 1]: "foo",
        [i32Max]: "foo",
        [i32Max + 1]: "foo",
        [u32Max - 1]: "foo",
        [u32Max]: "foo",
        [u32Max + 1]: "foo",
    };
    // Numeric properties come first in Object.getOwnPropertyNames()'s output,
    // which means we can test what each is treated as internally.
    expect(Object.getOwnPropertyNames(o)).toEqual([
        // Numeric properties
        "0",
        "1",
        "2147483646",
        "2147483647",
        "2147483648",
        "4294967294",
        // Non-numeric properties
        "-1",
        "4294967295", // >= 2^32 - 1
        "4294967296", // >= 2^32 - 1
    ]);
});

test("big int properties", () => {
    const o = {
        [-1n]: "foo",
        0n: "foo",
        1n: "foo",
        [12345678901n]: "foo",
        [4294967294n]: "foo",
        [4294967295n]: "foo",
    };
    // Numeric properties come first in Object.getOwnPropertyNames()'s output,
    // which means we can test what each is treated as internally.
    expect(Object.getOwnPropertyNames(o)).toEqual([
        // Numeric properties
        "0",
        "1",
        "4294967294",
        // Non-numeric properties
        "-1",
        "12345678901", // >= 2^32 - 1
        "4294967295", // >= 2^32 - 1
    ]);
});
