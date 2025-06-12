describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.getTimeZones();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").getTimeZones()).toBeUndefined();
        expect(new Intl.Locale("ar-Latn").getTimeZones()).toBeUndefined();

        const adZones = new Intl.Locale("en-AD").getTimeZones();
        expect(Array.isArray(adZones)).toBeTrue();
        expect(adZones).toEqual(["Europe/Andorra"]);

        const esZones = new Intl.Locale("en-ES").getTimeZones();
        expect(Array.isArray(esZones)).toBeTrue();
        expect(esZones).toEqual(["Africa/Ceuta", "Atlantic/Canary", "Europe/Madrid"]);
    });

    test("zone list is sorted", () => {
        const zones = new Intl.Locale("en-US").getTimeZones();
        const sortedZones = zones.toSorted();

        expect(zones).toEqual(sortedZones);
    });

    test("invalid region produces empty list", () => {
        const zones = new Intl.Locale("en-ZZ").getTimeZones();
        expect(Array.isArray(zones)).toBeTrue();
        expect(zones).toEqual([]);
    });
});
