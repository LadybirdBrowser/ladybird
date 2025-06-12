export function returnsOne() {
    return 1;
}

export class hasStaticFieldTwo {
    static two = 2;
}

const expectedValue = 10;
const didNotHoistClass = (() => {
    try {
        new ShouldNotBeHoisted();
    } catch (e) {
        if (e instanceof ReferenceError) return 4;
    }
    return 0;
})();

export const passed =
    returnsOne() + hasStaticFieldTwo.two + shouldBeHoisted() + didNotHoistClass === expectedValue;

export function shouldBeHoisted() {
    return 3;
}

export class ShouldNotBeHoisted {
    static no = 5;
}
