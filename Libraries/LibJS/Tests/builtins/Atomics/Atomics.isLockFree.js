test("invariants", () => {
    expect(Atomics.isLockFree).toHaveLength(1);
});

test("basic functionality", () => {
    expect(Atomics.isLockFree(4)).toBeTrue();

    // Can't assume the return value of sizes 1, 2, and 8, but they shouldn't change.
    expect(Atomics.isLockFree(1)).toBe(Atomics.isLockFree(1));
    expect(Atomics.isLockFree(2)).toBe(Atomics.isLockFree(2));
    expect(Atomics.isLockFree(8)).toBe(Atomics.isLockFree(8));

    expect(Atomics.isLockFree(0)).toBeFalse();
    expect(Atomics.isLockFree(3)).toBeFalse();
    expect(Atomics.isLockFree(5)).toBeFalse();
    expect(Atomics.isLockFree(6)).toBeFalse();
    expect(Atomics.isLockFree(7)).toBeFalse();
    expect(Atomics.isLockFree(9)).toBeFalse();
    expect(Atomics.isLockFree(10)).toBeFalse();
    expect(Atomics.isLockFree(Infinity)).toBeFalse();

    expect(Atomics.isLockFree("not a number")).toBeFalse();
    expect(Atomics.isLockFree(new Float32Array(4))).toBeFalse();
    expect(Atomics.isLockFree(String.prototype)).toBeFalse();
});
