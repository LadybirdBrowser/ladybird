test("length is 1", () => {
    expect(Array.prototype.filter).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].filter(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("never calls callback with empty array", () => {
        var callbackCalled = 0;
        expect(
            [].filter(() => {
                callbackCalled++;
            })
        ).toEqual([]);
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        expect(
            [1, 2, 3].filter(() => {
                callbackCalled++;
            })
        ).toEqual([]);
        expect(callbackCalled).toBe(3);
    });

    test("can filter based on callback return value", () => {
        var evenNumbers = [0, 1, 2, 3, 4, 5, 6, 7].filter(x => x % 2 === 0);
        expect(evenNumbers).toEqual([0, 2, 4, 6]);

        var fruits = [
            "Apple",
            "Banana",
            "Blueberry",
            "Grape",
            "Mango",
            "Orange",
            "Peach",
            "Pineapple",
            "Raspberry",
            "Watermelon",
        ];
        const filterItems = (arr, query) => {
            return arr.filter(el => el.toLowerCase().indexOf(query.toLowerCase()) !== -1);
        };
        expect(filterItems(fruits, "Berry")).toEqual(["Blueberry", "Raspberry"]);
        expect(filterItems(fruits, "P")).toEqual([
            "Apple",
            "Grape",
            "Peach",
            "Pineapple",
            "Raspberry",
        ]);
    });
});
