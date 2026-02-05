(function(root){
'use strict';
// testharness doesn't know about async test queues,
// so this wrapper takes care of that

/* USAGE:
    runParallelAsyncHarness({
        // list of data to test, must be array of objects.
        // each object must contain a "name" property to describe the test
        // besides name, the object can contain whatever data you need
        tests: [
            {name: "name of test 1", custom: "data"},
            {name: "name of test 2", custom: "data"},
            // ...
        ],

        // number of tests (tests, not test-cases!) to run concurrently
        testsPerSlice: 100,

        // time in milliseconds a test-run takes
        duration: 1000,

        // test-cases to run for for the test - there must be at least one
        // each case creates its separate async_test() instance
        cases: {
            // test case named "test1"
            test1: {
                // run as a async_test.step() this callback contains your primary assertions
                start: function(testCaseKey, data, options){},
                // run as a async_test.step() this callback contains assertions to be run
                // when the test ended, immediately before teardown
                done: function(testCaseKey, data, options){}
            },
            // ...
        }

        // all callbacks are optional:

        // invoked for individual test before it starts so you can setup the environment
        // like DOM, CSS, adding event listeners and such
        setup: function(data, options){},

        // invoked after a test ended, so you can clean up the environment
        // like DOM, CSS, removing event listeners and such
        teardown: function(data, options){},

        // invoked before a batch of tests ("slice") are run concurrently
        // tests is an array of test data objects
        sliceStart: function(options, tests)

        // invoked after a batch of tests ("slice") were run concurrently
        // tests is an array of test data objects
        sliceDone: function(options, tests)

        // invoked once all tests are done
        done: function(options){}
    })
*/

function DomContentLoadedPromise() {
    return new Promise(resolve => {
        document.addEventListener("DOMContentLoaded", (event) => {
            resolve();
        });
    });
}

root.runParallelAsyncHarness = async function(options) {
    const ready = DomContentLoadedPromise();

    if (!options.cases) {
        throw new Error("Options don't contain test cases!");
    }

    var noop = function(){};

    // names of individual tests
    var cases = Object.keys(options.cases);

    // run tests in a batch of slices
    // primarily not to overload weak devices (tablets, phones, …)
    // with too many tests running simultaneously
    var iteration = -1;
    var testPerSlice = options.testsPerSlice || 100;
    var slices = Math.ceil(options.tests.length / testPerSlice);

    // initialize all async test cases
    // Note: satisfying testharness.js needs to know all async tests before load-event
    options.tests.forEach(function(data, index) {
        data.cases = {};
        cases.forEach(function(name) {
            data.cases[name] = async_test(data.name + " / " + name);
        });
    });

    function runLoop() {
        iteration++;
        if (iteration >= slices) {
            // no more slice, we're done
            (options.done || noop)(options);
            return;
        }

        // grab a slice of testss and initialize them
        var offset = iteration * testPerSlice;
        var tests = options.tests.slice(offset, offset + testPerSlice);
        tests.forEach(function(data) {
            (options.setup || noop)(data, options);
        });

        // kick off the current slice of tests
        (options.sliceStart || noop)(options, tests);

        // perform individual "start" test-case
        tests.forEach(function(data) {
            cases.forEach(function(name) {
                data.cases[name].step(function() {
                    (options.cases[name].start || noop)(data.cases[name], data, options);
                });
            });
        });

        // Start sampling.
        (options.transitionsStarted || noop)(options, tests);

        // conclude slice (possibly abort)
        var concludeSlice = function() {
            tests.forEach(function(data) {
                // perform individual "done" test-case
                cases.forEach(function(name) {
                    data.cases[name].step(function() {
                        (options.cases[name].done || noop)(data.cases[name], data, options);
                    });
                });
                // clean up after individual test
                (options.teardown || noop)(data, options);
                // tell harness we're done with individual test-cases
                cases.forEach(function(name) {
                    data.cases[name].done();
                });
            });

            // finish the test for current slice of tests
            (options.sliceDone || noop)(options, tests);

            requestAnimationFrame(runLoop);
        }

        options.allTransitionsCompleted = concludeSlice;
    }

    // allow DOMContentLoaded before actually doing something
    ready.then(runLoop);
};

})(window);
