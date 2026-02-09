function destructure() {
    let [a, b] = [1, 2];
    let { x, y: z } = { x: 3, y: 4 };
    return a + b + x + z;
}
