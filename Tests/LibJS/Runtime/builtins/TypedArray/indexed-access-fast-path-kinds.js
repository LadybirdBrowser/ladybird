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
