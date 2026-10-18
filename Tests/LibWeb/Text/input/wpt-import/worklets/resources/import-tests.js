// Runs a series of tests related to importing scripts on a worklet.
//
// Usage:
// runImportTests("paint");
function runImportTests(worklet_type) {
    const worklet = get_worklet(worklet_type);

    promise_test(() => {
        const kScriptURL = 'resources/empty-worklet-script.js';
        return worklet.addModule(kScriptURL).then(undefined_arg => {
            assert_equals(undefined_arg, undefined,
                          'Promise should resolve with no arguments.');
        });
    }, 'Importing a script resolves the given promise.');

    promise_test(() => {
        const kScriptURL = 'resources/empty-worklet-script.js';
        return Promise.all([
            worklet.addModule(kScriptURL + '?1'),
            worklet.addModule(kScriptURL + '?2'),
            worklet.addModule(kScriptURL + '?3')
        ]).then(undefined_args => {
            assert_array_equals(undefined_args,
                                [undefined, undefined, undefined],
                                'Promise should resolve with no arguments.');
        });
    }, 'Importing scripts resolves all the given promises.');

    promise_test(() => {
        const kScriptURL = 'resources/import-nested-worklet-script.js';
        return worklet.addModule(kScriptURL).then(undefined_arg => {
            assert_equals(undefined_arg, undefined,
                          'Promise should resolve with no arguments.');
        });
    }, 'Importing nested scripts resolves the given promise');

    promise_test(() => {
        const kScriptURL = 'resources/import-cyclic-worklet-script.js';
        return worklet.addModule(kScriptURL).then(undefined_arg => {
            assert_equals(undefined_arg, undefined,
                          'Promise should resolve with no arguments.');
        });
    }, 'Importing cyclic scripts resolves the given promise');

    promise_test(() => {
        const kScriptURL = 'resources/throwing-worklet-script.js';
        return worklet.addModule(kScriptURL).then(undefined_arg => {
            assert_equals(undefined_arg, undefined,
                          'Promise should resolve with no arguments.');
        });
    }, 'Importing a script which throws should still resolve the given ' +
       'promise.');

    promise_test(t => {
        const kScriptURL = 'non-existent-worklet-script.js';
        return promise_rejects_dom(t, 'AbortError',
                                   worklet.addModule(kScriptURL));
    }, 'Importing a non-existent script rejects the given promise with an ' +
       'AbortError.');

    promise_test(t => {
        const kInvalidScriptURL = 'http://invalid:123$'
        return promise_rejects_dom(t, 'SyntaxError',
                                   worklet.addModule(kInvalidScriptURL));
    }, 'Importing an invalid URL should reject the given promise with a ' +
       'SyntaxError.');

    promise_test(() => {
        const kBlob = new Blob([""], {type: 'text/javascript'});
        const kBlobURL = URL.createObjectURL(kBlob);
        return worklet.addModule(kBlobURL).then(undefined_arg => {
            assert_equals(undefined_arg, undefined);
        });
    }, 'Importing a blob URL should resolve the given promise.');

    promise_test(t => {
        const kFileURL = 'file:///empty-worklet-script.js';
        return promise_rejects_dom(t, 'AbortError',
                                   worklet.addModule(kFileURL));
    }, 'Importing a file:// URL should reject the given promise.');

    promise_test(() => {
        const kDataURL = 'data:text/javascript, // Do nothing.';
        return worklet.addModule(kDataURL).then(undefined_arg => {
            assert_equals(undefined_arg, undefined);
        });
    }, 'Importing a data URL should resolve the given promise.');

    promise_test(t => {
        const kScriptURL = 'about:blank';
        return promise_rejects_dom(t, 'AbortError',
                                   worklet.addModule(kScriptURL));
    }, 'Importing about:blank should reject the given promise.');

    promise_test(() => {
        // Specify the Access-Control-Allow-Origin header to enable cross origin
        // access.
        const kScriptURL = get_host_info().HTTPS_REMOTE_ORIGIN +
                           '/worklets/resources/empty-worklet-script.js' +
                           '?pipe=header(Access-Control-Allow-Origin, *)';
        return worklet.addModule(kScriptURL).then(undefined_arg => {
            assert_equals(undefined_arg, undefined);
        });
    }, 'Importing a cross origin resource with the ' +
       'Access-Control-Allow-Origin header should resolve the given promise');

    promise_test(t => {
        // Don't specify the Access-Control-Allow-Origin header. addModule()
        // should be rejected because of disallowed cross origin access.
        const kScriptURL = get_host_info().HTTPS_REMOTE_ORIGIN +
                           '/worklets/resources/empty-worklet-script.js';
        return promise_rejects_dom(t, 'AbortError',
                                   worklet.addModule(kScriptURL));
    }, 'Importing a cross origin resource without the ' +
       'Access-Control-Allow-Origin header should reject the given promise');

    promise_test(() => {
        const kScriptURL = get_host_info().HTTPS_REMOTE_ORIGIN +
                           '/worklets/resources/empty-worklet-script.js' +
                           '?pipe=header(Access-Control-Allow-Origin, ' +
                           location.origin + ')';
        return worklet.addModule('/common/redirect.py?location=' +
                                 encodeURIComponent(kScriptURL))
          .then(undefined_arg => {
              assert_equals(undefined_arg, undefined);
          });
    }, 'Importing a cross-origin-redirected resource with the ' +
       'Access-Control-Allow-Origin header should resolve the given promise');

    promise_test(t => {
        const kScriptURL = get_host_info().HTTPS_REMOTE_ORIGIN +
                           '/worklets/resources/empty-worklet-script.js';
        return promise_rejects_dom(t, 'AbortError',
                                   worklet.addModule(
                                       '/common/redirect.py?location=' +
                                       encodeURIComponent(kScriptURL)));
    }, 'Importing a cross-origin-redirected resource without the ' +
       'Access-Control-Allow-Origin header should reject the given promise');

    promise_test(t => {
        const kScriptURL = 'resources/syntax-error-worklet-script.js';
        return promise_rejects_js(t, SyntaxError,
                                  worklet.addModule(kScriptURL));
    }, 'Importing a script that has a syntax error should reject the given ' +
       'promise.');

    promise_test(t => {
        const kScriptURL = 'resources/import-syntax-error-worklet-script.js';
        return promise_rejects_js(t, SyntaxError,
                                  worklet.addModule(kScriptURL));
    }, 'Importing a nested script that has a syntax error should reject the ' +
       'given promise.');

    promise_test(t => {
        const kBlob = new Blob(["import 'invalid-specifier.js';"],
                               {type: 'text/javascript'});
        const kBlobURL = URL.createObjectURL(kBlob);
        return promise_rejects_js(t, TypeError,
                                  worklet.addModule(kBlobURL));
    }, 'Importing a script that imports an invalid identifier should reject ' +
       'the given promise.');
}
