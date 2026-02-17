class C {
    #x = 1;
    get #y() {
        return this.#x;
    }
    set #y(v) {
        this.#x = v;
    }
    method() {
        return this.#x;
    }
}
