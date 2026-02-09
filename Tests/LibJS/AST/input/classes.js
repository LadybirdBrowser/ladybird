class Animal {
    constructor(name) {
        this.name = name;
    }

    speak() {
        return this.name;
    }

    get info() {
        return this.name;
    }
}

class Dog extends Animal {
    constructor(name) {
        super(name);
    }
}
