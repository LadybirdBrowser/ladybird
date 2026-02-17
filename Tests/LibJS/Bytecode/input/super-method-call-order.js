var proto = { m() {} };
var obj = {
    __proto__: proto,
    test() {
        return super.m();
    },
    get x() {
        return super.m();
    },
    test2() {
        return super["m"]();
    },
};
obj.test();
obj.x;
obj.test2();
