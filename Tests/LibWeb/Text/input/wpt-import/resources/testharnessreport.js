/* global add_completion_callback */
/* global setup */

/*
 * This file is intended for vendors to implement code needed to integrate
 * testharness.js tests with their own test systems.
 *
 * Typically test system integration will attach callbacks when each test has
 * run, using add_result_callback(callback(test)), or when the whole test file
 * has completed, using
 * add_completion_callback(callback(tests, harness_status)).
 *
 * For more documentation about the callback functions and the
 * parameters they are called with see testharness.js
 */

/* If the parent window has a testharness_properties object,
 * we use this to provide the test settings. This is used by the
 * default in-browser runner to configure the timeout and the
 * rendering of results
 */
try {
    if (window.opener && "testharness_properties" in window.opener) {
        /* If we pass the testharness_properties object as-is here without
         * JSON stringifying and reparsing it, IE fails & emits the message
         * "Could not complete the operation due to error 80700019".
         */
        setup(JSON.parse(JSON.stringify(window.opener.testharness_properties)));
    }
} catch (e) {
}

add_completion_callback(function(tests, harness_status) {
    if (window.internals) {
        const statuses = [
            "Pass",
            "Fail",
            "Timeout",
            "Not Run",
            "Optional Feature Unsupported",
        ];
        let outputLines = [];
        outputLines.push(`Harness status: ${harness_status.format_status()}`);
        outputLines.push("");
        outputLines.push(`Found ${tests.length} tests`);
        outputLines.push("");
        for (let i = 0; i < statuses.length; ++i) {
            let count = tests.filter(test => test.status === i).length;
            if (count > 0) {
                outputLines.push(`${count} ${statuses[i]}`);
            }
        }
        for (const test of tests) {
            outputLines.push(`${test.format_status()}\t${test.name}`);
        }
        window.internals.signalTestIsDone(outputLines.join('\n'));
    }
});


// vim: set expandtab shiftwidth=4 tabstop=4:
