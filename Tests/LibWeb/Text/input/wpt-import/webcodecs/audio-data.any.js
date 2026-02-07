// META: global=window,dedicatedworker
// META: script=/common/media.js
// META: script=/webcodecs/utils.js

var defaultInit = {
    timestamp: 1234,
    channels: 2,
    sampleRate: 8000,
    frames: 100,
};

function createDefaultAudioData() {
    return make_audio_data(
        defaultInit.timestamp,
        defaultInit.channels,
        defaultInit.sampleRate,
        defaultInit.frames
    );
}

test(t => {
    let local_data = new Float32Array(defaultInit.channels * defaultInit.frames);

    let audio_data_init = {
        timestamp: defaultInit.timestamp,
        data: local_data,
        numberOfFrames: defaultInit.frames,
        numberOfChannels: defaultInit.channels,
        sampleRate: defaultInit.sampleRate,
        format: 'f32-planar',
    }

    let data = new AudioData(audio_data_init);

    assert_equals(data.timestamp, defaultInit.timestamp, 'timestamp');
    assert_equals(data.numberOfFrames, defaultInit.frames, 'frames');
    assert_equals(data.numberOfChannels, defaultInit.channels, 'channels');
    assert_equals(data.sampleRate, defaultInit.sampleRate, 'sampleRate');
    assert_equals(
        data.duration, defaultInit.frames / defaultInit.sampleRate * 1_000_000,
        'duration');
    assert_equals(data.format, 'f32-planar', 'format');

    // Create an Int16 array of the right length.
    let small_data = new Int16Array(defaultInit.channels * defaultInit.frames);

    let wrong_format_init = {...audio_data_init};
    wrong_format_init.data = small_data;

    // Creating `f32-planar` AudioData from Int16 from should throw.
    assert_throws_js(TypeError, () => {
        let data = new AudioData(wrong_format_init);
    }, `AudioDataInit.data needs to be big enough`);

    var members = [
        'timestamp',
        'data',
        'numberOfFrames',
        'numberOfChannels',
        'sampleRate',
        'format',
    ];

    for (const member of members) {
        let incomplete_init = {...audio_data_init};
        delete incomplete_init[member];

        assert_throws_js(
            TypeError, () => {let data = new AudioData(incomplete_init)},
            'AudioData requires \'' + member + '\'');
    }

    let invalid_init = {...audio_data_init};
    invalid_init.numberOfFrames = 0

    assert_throws_js(
        TypeError, () => {let data = new AudioData(invalid_init)},
        'AudioData requires numberOfFrames > 0');

    invalid_init = {...audio_data_init};
    invalid_init.numberOfChannels = 0

    assert_throws_js(
        TypeError, () => {let data = new AudioData(invalid_init)},
        'AudioData requires numberOfChannels > 0');

}, 'Verify AudioData constructors');

test(t => {
    let data = createDefaultAudioData();
    data.close();
    assert_equals(data.sampleRate, 0);
    assert_equals(data.numberOfFrames, 0);
    assert_equals(data.numberOfChannels, 0);
    assert_equals(data.format, null);
}, 'AudioData close');

test(t => {
    let data = createDefaultAudioData();

    let clone = data.clone();

    // Verify the parameters match.
    assert_equals(data.timestamp, clone.timestamp, 'timestamp');
    assert_equals(data.numberOfFrames, clone.numberOfFrames, 'frames');
    assert_equals(data.numberOfChannels, clone.numberOfChannels, 'channels');
    assert_equals(data.sampleRate, clone.sampleRate, 'sampleRate');
    assert_equals(data.format, clone.format, 'format');

    const data_copyDest = new Float32Array(defaultInit.frames);
    const clone_copyDest = new Float32Array(defaultInit.frames);

    // Verify the data matches.
    for (var channel = 0; channel < defaultInit.channels; channel++) {
        data.copyTo(data_copyDest, {planeIndex: channel});
        clone.copyTo(clone_copyDest, {planeIndex: channel});

        assert_array_equals(
            data_copyDest, clone_copyDest, 'Cloned data ch=' + channel);
    }

    // Verify closing the original data doesn't close the clone.
    data.close();
    assert_equals(data.numberOfFrames, 0, 'data.buffer (closed)');
    assert_not_equals(clone.numberOfFrames, 0, 'clone.buffer (not closed)');

    clone.close();
    assert_equals(clone.numberOfFrames, 0, 'clone.buffer (closed)');

    // Verify closing a closed AudioData does not throw.
    data.close();
}, 'Verify closing and cloning AudioData');

test(t => {
    let data = make_audio_data(
        -10, defaultInit.channels, defaultInit.sampleRate, defaultInit.frames);
    assert_equals(data.timestamp, -10, 'timestamp');
    data.close();
}, 'Test we can construct AudioData with a negative timestamp.');

test(t => {
    var data = new Float32Array([0]);
    let audio_data_init = {
        timestamp: 0,
        data: data,
        numberOfFrames: 1,
        numberOfChannels: 1,
        sampleRate: 44100,
        format: 'f32',
    };
    let audioData = new AudioData(audio_data_init);
    assert_not_equals(data.length, 0, "Input data is copied when constructing an AudioData");
}, 'Test input array is copied on construction');

test(t => {
    let audio_data_init = {
        timestamp: 0,
        data: new Float32Array([1,2,3,4,5,6,7,8]),
        numberOfFrames: 4,
        numberOfChannels: 2,
        sampleRate: 44100,
        format: 'f32',
    };
    let audioData = new AudioData(audio_data_init);
    let dest = new Float32Array(8);
    assert_throws_js(
        RangeError, () => audioData.copyTo(dest, {planeIndex: 1}),
        'copyTo from interleaved data with non-zero planeIndex throws');
    audioData.close();
}, 'Test that copyTo throws if copying from interleaved with a non-zero planeIndex');

// Indices to pick a particular specific value in a specific sample-format
const MIN = 0; // Minimum sample value, max amplitude
const MAX = 1; // Maximum sample value, max amplitude
const HALF = 2; // Half the maximum sample value, positive
const NEGATIVE_HALF = 3; // Half the maximum sample value, negative
const BIAS = 4; // Center of the range, silence
const DISCRETE_STEPS = 5; // Number of different value for a type.

function pow2(p) {
    return 2 ** p;
}
// Rounding operations for conversion, currently always floor (round towards
// zero).
let r = Math.floor.bind(this);

const TEST_VALUES = {
    u8: [0, 255, 191, 64, 128, 256],
    s16: [
        -pow2(15),
        pow2(15) - 1,
        r((pow2(15) - 1) / 2),
        r(-pow2(15) / 2),
        0,
        pow2(16),
    ],
    s32: [
        -pow2(31),
        pow2(31) - 1,
        r((pow2(31) - 1) / 2),
        r(-pow2(31) / 2),
        0,
        pow2(32),
    ],
    f32: [-1.0, 1.0, 0.5, -0.5, 0, pow2(24)],
};

const TEST_TEMPLATE = {
    channels: 2,
    frames: 5,
    // Each test is run with an element of the cartesian product of a pair of
    // elements of the set of type in [u8, s16, s32, f32]
    // For each test, this template is copied and the values replaced with the
    // appropriate values for this particular type.
    // For each test, copy this template and replace the number by the appropriate
    // number for this type
    testInput: [MIN, BIAS, MAX, MIN, HALF, NEGATIVE_HALF, BIAS, MAX, BIAS, BIAS],
    testInterleavedResult: [MIN, NEGATIVE_HALF, BIAS, BIAS, MAX, MAX, MIN, BIAS, HALF, BIAS],
    testVectorInterleavedResult: [
        [MIN, MAX, HALF, BIAS, BIAS],
        [BIAS, MIN, NEGATIVE_HALF, MAX, BIAS],
    ],
    testVectorPlanarResult: [
        [MIN, BIAS, MAX, MIN, HALF],
        [NEGATIVE_HALF, BIAS, MAX, BIAS, BIAS],
    ],
};

function isInteger(type) {
    switch (type) {
        case "u8":
        case "s16":
        case "s32":
            return true;
        case "f32":
            return false;
        default:
            throw "invalid type";
    }
}

// This is the complex part: carefully select an acceptable error value
// depending on various factors: expected destination value, source type,
// destination type. This is designed to be strict but reachable with simple
// sample format transformation (no dithering or complex transformation).
function epsilon(expectedDestValue, sourceType, destType) {
    // Strict comparison if not converting
    if (sourceType == destType) {
        return 0.0;
    }
    // There are three cases in which the maximum value cannot be reached, when
    // converting from a smaller integer sample type to a wider integer sample
    // type:
    // - u8 to s16
    // - u8 to s32
    // - s16 to u32
    if (expectedDestValue == TEST_VALUES[destType][MAX]) {
        if (sourceType == "u8" && destType == "s16") {
            return expectedDestValue - 32511; // INT16_MAX - 2 << 7 + 1
        } else if (sourceType == "u8" && destType == "s32") {
            return expectedDestValue - 2130706432; // INT32_MAX - (2 << 23) + 1
        } else if (sourceType == "s16" && destType == "s32") {
            return expectedDestValue - 2147418112; // INT32_MAX - UINT16_MAX
        }
    }
    // Min and bias value are correctly mapped for all integer sample-types
    if (isInteger(sourceType) && isInteger(destType)) {
        if (expectedDestValue == TEST_VALUES[destType][MIN] ||
            expectedDestValue == TEST_VALUES[destType][BIAS]) {
            return 0.0;
        }
    }
    // If converting from float32 to u8 or s16, allow choosing the rounding
    // direction. s32 has higher resolution than f32 in [-1.0,1.0] (24 bits of
    // mantissa)
    if (!isInteger(sourceType) && isInteger(destType) && destType != "s32") {
        return 1.0;
    }
    // In all other cases, expect an accuracy that depends on the source type and
    // the destination type.
    // The resolution of the source type.
    var sourceResolution = TEST_VALUES[sourceType][DISCRETE_STEPS];
    // The resolution of the destination type.
    var destResolution = TEST_VALUES[destType][DISCRETE_STEPS];
    // Computations should be exact if going from high resolution to low resolution.
    if (sourceResolution > destResolution) {
        return 0.0;
    } else {
        // Something that approaches the precision imbalance
        return destResolution / sourceResolution;
    }
}

// Fill the template above with the values for a particular type
function get_type_values(type) {
    let cloned = structuredClone(TEST_TEMPLATE);
    cloned.testInput = Array.from(
        cloned.testInput,
        idx => TEST_VALUES[type][idx]
    );
    cloned.testInterleavedResult = Array.from(
        cloned.testInterleavedResult,
        idx => TEST_VALUES[type][idx]
    );
    cloned.testVectorInterleavedResult = Array.from(
        cloned.testVectorInterleavedResult,
        c => {
            return Array.from(c, idx => {
                return TEST_VALUES[type][idx];
            });
        }
    );
    cloned.testVectorPlanarResult = Array.from(
        cloned.testVectorPlanarResult,
        c => {
            return Array.from(c, idx => {
                return TEST_VALUES[type][idx];
            });
        }
    );
    return cloned;
}

function typeToArrayType(type) {
    switch (type) {
        case "u8":
            return Uint8Array;
        case "s16":
            return Int16Array;
        case "s32":
            return Int32Array;
        case "f32":
            return Float32Array;
        default:
            throw "Unexpected";
    }
}

function arrayTypeToType(array) {
    switch (array.constructor) {
        case Uint8Array:
            return "u8";
        case Int16Array:
            return "s16";
        case Int32Array:
            return "s32";
        case Float32Array:
            return "f32";
        default:
            throw "Unexpected";
    }
}

function check_array_equality(values, expected, sourceType, message, assert_func) {
    if (values.length != expected.length) {
        throw "Array not of the same length";
    }
    for (var i = 0; i < values.length; i++) {
        var eps = epsilon(expected[i], sourceType, arrayTypeToType(values));
        assert_func(
            Math.abs(expected[i] - values[i]) <= eps,
            `Got ${values[i]} but expected result ${
                expected[i]
            } at index ${i} when converting from ${sourceType} to ${arrayTypeToType(
                values
            )}, epsilon ${eps}`
        );
    }
    assert_func(
        true,
        `${values} is equal to ${expected} when converting from ${sourceType} to ${arrayTypeToType(
            values
        )}`
    );
}

function conversionTest(sourceType, destinationType) {
    test(function (t) {
        var test = get_type_values(sourceType);
        var result = get_type_values(destinationType);

        var sourceArrayCtor = typeToArrayType(sourceType);
        var destArrayCtor = typeToArrayType(destinationType);

        let data = new AudioData({
            timestamp: defaultInit.timestamp,
            data: new sourceArrayCtor(test.testInput),
            numberOfFrames: test.frames,
            numberOfChannels: test.channels,
            sampleRate: defaultInit.sampleRate,
            format: sourceType,
        });

        // All conversions can be supported, but conversion of any type to f32-planar
        // MUST be supported.
        var assert_func = destinationType == "f32" ? assert_true : assert_implements_optional;
        let dest = new destArrayCtor(data.numberOfFrames);
        data.copyTo(dest, { planeIndex: 0, format: destinationType + "-planar" });
        check_array_equality(
            dest,
            result.testVectorInterleavedResult[0],
            sourceType,
            "interleaved channel 0",
            assert_func
        );
        data.copyTo(dest, { planeIndex: 1, format: destinationType + "-planar" });
        check_array_equality(
            dest,
            result.testVectorInterleavedResult[1],
            sourceType,
            "interleaved channel 0",
            assert_func
        );
        let destInterleaved = new destArrayCtor(data.numberOfFrames * data.numberOfChannels);
        data.copyTo(destInterleaved, { planeIndex: 0, format: destinationType });
        check_array_equality(
            destInterleaved,
            result.testInput,
            sourceType,
            "copyTo from interleaved to interleaved (conversion only)",
            assert_implements_optional
        );

        data = new AudioData({
            timestamp: defaultInit.timestamp,
            data: new sourceArrayCtor(test.testInput),
            numberOfFrames: test.frames,
            numberOfChannels: test.channels,
            sampleRate: defaultInit.sampleRate,
            format: sourceType + "-planar",
        });

        data.copyTo(dest, { planeIndex: 0, format: destinationType + "-planar" });
        check_array_equality(
            dest,
            result.testVectorPlanarResult[0],
            sourceType,
            "planar channel 0",
            assert_func,
        );
        data.copyTo(dest, { planeIndex: 1, format: destinationType + "-planar" });
        check_array_equality(
            dest,
            result.testVectorPlanarResult[1],
            sourceType,
            "planar channel 1",
            assert_func
        );
        // Copy to interleaved from planar: all channels are copied
        data.copyTo(destInterleaved, {planeIndex: 0, format: destinationType});
        check_array_equality(
            destInterleaved,
            result.testInterleavedResult,
            sourceType,
            "planar to interleaved",
            assert_func
        );
    }, `Test conversion of ${sourceType} to ${destinationType}`);
}

const TYPES = ["u8", "s16", "s32", "f32"];
TYPES.forEach(sourceType => {
    TYPES.forEach(destinationType => {
        conversionTest(sourceType, destinationType);
    });
});
