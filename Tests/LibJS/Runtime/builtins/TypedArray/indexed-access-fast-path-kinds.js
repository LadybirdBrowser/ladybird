test("Uint8ClampedArray direct indexed stores clamp int32 values", () => {
    let array = new Uint8ClampedArray(4);

    array[0] = -123;
    array[1] = 42;
    array[2] = 255;
    array[3] = 999;

    expect(array[0]).toBe(0);
    expect(Reflect.get(array, 0)).toBe(0);
    expect(array[1]).toBe(42);
    expect(Reflect.get(array, 1)).toBe(42);
    expect(array[2]).toBe(255);
    expect(Reflect.get(array, 2)).toBe(255);
    expect(array[3]).toBe(255);
    expect(Reflect.get(array, 3)).toBe(255);
});

test("Float32Array direct indexed access handles int32 and double values", () => {
    let array = new Float32Array(4);

    array[0] = 1;
    array[1] = Math.PI;
    array[2] = -0;
    array[3] = NaN;

    expect(array[0]).toBe(1);
    expect(Reflect.get(array, 0)).toBe(1);
    expect(array[1]).toBe(Math.fround(Math.PI));
    expect(Reflect.get(array, 1)).toBe(Math.fround(Math.PI));
    expect(Object.is(array[2], -0)).toBeTrue();
    expect(Object.is(Reflect.get(array, 2), -0)).toBeTrue();
    expect(Number.isNaN(array[3])).toBeTrue();
    expect(Number.isNaN(Reflect.get(array, 3))).toBeTrue();
});
