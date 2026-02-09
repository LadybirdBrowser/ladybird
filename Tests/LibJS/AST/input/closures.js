function outer(x) {
    let y = 10;
    function inner(z) {
        return x + y + z;
    }
    return inner(5);
}
