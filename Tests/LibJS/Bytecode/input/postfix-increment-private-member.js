class C {
    #c = 0;
    nextId() {
        return "id-" + this.#c++;
    }
}
new C().nextId();
