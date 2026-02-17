class C {
    #m() {
        return 42;
    }
    call() {
        return this.#m();
    }
}
new C().call();
