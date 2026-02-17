function foo() {
    return (() => this.x + this.y)();
}
foo();
