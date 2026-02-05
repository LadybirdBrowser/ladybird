test("basic functionality", () => {
    expect(Math.atan2).toHaveLength(2);

    expect(Math.atan2(90, 15)).toBeCloseTo(1.4056476493802699);
    expect(Math.atan2(15, 90)).toBeCloseTo(0.16514867741462683);
    expect(Math.atan2(+0, -0)).toBeCloseTo(Math.PI);
    expect(Math.atan2(-0, -0)).toBeCloseTo(-Math.PI);
    expect(Math.atan2(+0, +0)).toBe(0);
    expect(Math.atan2(-0, +0)).toBe(-0);
    expect(Math.atan2(+0, -1)).toBeCloseTo(Math.PI);
    expect(Math.atan2(-0, -1)).toBeCloseTo(-Math.PI);
    expect(Math.atan2(+0, 1)).toBe(0);
    expect(Math.atan2(-0, 1)).toBe(-0);
    expect(Math.atan2(-1, +0)).toBeCloseTo(-Math.PI / 2);
    expect(Math.atan2(-1, -0)).toBeCloseTo(-Math.PI / 2);
    expect(Math.atan2(1, +0)).toBeCloseTo(Math.PI / 2);
    expect(Math.atan2(1, -0)).toBeCloseTo(Math.PI / 2);
    expect(Math.atan2(1, -Infinity)).toBeCloseTo(Math.PI);
    expect(Math.atan2(-1, -Infinity)).toBeCloseTo(-Math.PI);
    expect(Math.atan2(1, +Infinity)).toBe(0);
    expect(Math.atan2(-1, +Infinity)).toBe(-0);
    expect(Math.atan2(+Infinity, 1)).toBeCloseTo(Math.PI / 2);
    expect(Math.atan2(-Infinity, 1)).toBeCloseTo(-Math.PI / 2);
    expect(Math.atan2(+Infinity, -Infinity)).toBeCloseTo((3 * Math.PI) / 4);
    expect(Math.atan2(-Infinity, -Infinity)).toBeCloseTo((-3 * Math.PI) / 4);
    expect(Math.atan2(+Infinity, +Infinity)).toBeCloseTo(Math.PI / 4);
    expect(Math.atan2(-Infinity, +Infinity)).toBeCloseTo(-Math.PI / 4);
});
