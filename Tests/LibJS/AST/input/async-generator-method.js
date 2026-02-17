let obj = {
    async *foo() {
        yield 1;
    },
};

class C {
    async *bar() {
        yield 2;
    }
}
