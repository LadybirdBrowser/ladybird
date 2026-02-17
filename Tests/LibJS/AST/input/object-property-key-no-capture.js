function outer() {
    var x = 1;
    function inner() {
        return { x: 42 };
    }
    return x;
}
