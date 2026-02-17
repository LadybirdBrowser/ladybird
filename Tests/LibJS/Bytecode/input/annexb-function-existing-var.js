function test() {
    var f = 123;
    if (true) {
        function f() {
            return 42;
        }
    }
    return f;
}
test();
