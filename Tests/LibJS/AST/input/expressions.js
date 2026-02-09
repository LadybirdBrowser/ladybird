function expressions(a, b) {
    let x = a + b * 2;
    let y = typeof a === "number";
    let z = a > 0 ? a : -a;
    a++;
    return x + y + z;
}
