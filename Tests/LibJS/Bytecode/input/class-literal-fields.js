function test() {
    class A {
        x = 42;
        y = -1;
        z = true;
        w = null;
        s = "hi";
        computed = 1 + 2;
    }
    return new A();
}
let a = test();
console.log(a.x, a.y, a.z, a.w, a.s, a.computed);
