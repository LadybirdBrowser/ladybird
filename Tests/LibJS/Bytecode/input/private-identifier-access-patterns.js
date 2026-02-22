class C {
    #x;
    method() {
        this.#x;
        this?.#x;
        this?.foo.#x;
        #x in this;
    }
}
