'use strict';

// These tests rely on the User Agent providing an implementation of the
// WebXR Testing API (https://github.com/immersive-web/webxr-test-api).
//
// In Chromium-based browsers, this implementation is provided by a JavaScript
// shim in order to reduce the amount of test-only code shipped to users. To
// enable these tests the browser must be run with these options:
//
//   --enable-blink-features=MojoJS,MojoJSTest

// Debugging message helper, by default does nothing. Implementations can
// override this.
var xr_debug = function(name, msg) {};

let loaded = new Promise(resolve => document.addEventListener('DOMContentLoaded', resolve));

function xr_promise_test(name, func, properties, glContextType, glContextProperties) {
  promise_test(async (t) => {
    if (glContextType === 'webgl2') {
      // Fast fail on platforms not supporting WebGL2.
      assert_implements('WebGL2RenderingContext' in window, 'webgl2 not supported.');
    }
    // Perform any required test setup:
    xr_debug(name, 'setup');

    assert_implements(navigator.xr, 'missing navigator.xr - ensure test is run in a secure context.');

    // Only set up once.
    if (!navigator.xr.test) {

      if (typeof isChromiumBased === 'undefined' || typeof isWebKitBased === 'undefined') {
        // Load test-only API helpers.
        const script = document.createElement('script');
        script.src = '/resources/test-only-api.js';
        script.async = false;
        const p = new Promise((resolve, reject) => {
          script.onload = () => { resolve(); };
          script.onerror = e => { reject(e); };
        });
        document.head.appendChild(script);
        await p;
      }

      if (isChromiumBased) {
        // Chrome setup
        await loadChromiumResources();
      } else if (isWebKitBased) {
        // WebKit setup
        await setupWebKitWebXRTestAPI();
      }
    }

    // Either the test api needs to be polyfilled and it's not set up above, or
    // something happened to one of the known polyfills and it failed to be
    // setup properly. Either way, the fact that xr_promise_test is being used
    // means that the tests expect navigator.xr.test to be set. By rejecting now
    // we can hopefully provide a clearer indication of what went wrong.
    assert_implements(navigator.xr.test, 'missing navigator.xr.test, even after attempted load');

    let gl = null;
    let canvas = null;
    if (glContextType) {
      canvas = document.createElement('canvas');
      await loaded;
      document.body.appendChild(canvas);
      gl = canvas.getContext(glContextType, glContextProperties);
    }

    // Ensure that any devices are disconnected when done. If this were done in
    // a .then() for the success case, a test that expected failure would
    // already be marked done at the time that runs and the shutdown would
    // interfere with the next test.
    t.add_cleanup(async () => {
      // Ensure system state is cleaned up.
      xr_debug(name, 'cleanup');
      await navigator.xr.test.disconnectAllDevices();
    });

    xr_debug(name, 'main');
    return func(t, gl);
  }, name, properties);
}

// A utility function for waiting one animation frame before running the callback
//
// This is only needed after calling FakeXRDevice methods outside of an animation frame
//
// This is so that we can paper over the potential race allowed by the "next animation frame"
// concept https://immersive-web.github.io/webxr-test-api/#xrsession-next-animation-frame
function requestSkipAnimationFrame(session, callback) {
 session.requestAnimationFrame(() => {
  session.requestAnimationFrame(callback);
 });
}

// A test function which runs through the common steps of requesting a session.
// Calls the passed in test function with the session, the controller for the
// device, and the test object.
function xr_session_promise_test(
    name, func, fakeDeviceInit, sessionMode, sessionInit, properties,
    glcontextPropertiesParam, gllayerPropertiesParam) {
  const glcontextProperties = (glcontextPropertiesParam) ? glcontextPropertiesParam : {};
  const gllayerProperties = (gllayerPropertiesParam) ? gllayerPropertiesParam : {};

  function runTest(t, glContext) {
    let testSession;
    let testDeviceController;
    let sessionObjects = {gl: glContext};

    // Ensure that any pending sessions are ended when done. This needs to
    // use a cleanup function to ensure proper sequencing. If this were
    // done in a .then() for the success case, a test that expected
    // failure would already be marked done at the time that runs, and the
    // shutdown would interfere with the next test which may have started.
    t.add_cleanup(async () => {
      // If a session was created, end it.
      if (testSession) {
        await testSession.end().catch(() => {});
      }
    });

    return navigator.xr.test.simulateDeviceConnection(fakeDeviceInit)
        .then((controller) => {
          testDeviceController = controller;
          return sessionObjects.gl.makeXRCompatible();
        })
        .then(() => new Promise((resolve, reject) => {
                // Perform the session request in a user gesture.
                xr_debug(name, 'simulateUserActivation');
                navigator.xr.test.simulateUserActivation(() => {
                  xr_debug(name, 'document.hasFocus()=' + document.hasFocus());
                  navigator.xr.requestSession(sessionMode, sessionInit || {})
                      .then((session) => {
                        xr_debug(name, 'session start');
                        testSession = session;
                        session.mode = sessionMode;
                        session.sessionInit = sessionInit;
                        let glLayer = new XRWebGLLayer(session, sessionObjects.gl, gllayerProperties);
                        glLayer.context = sessionObjects.gl;
                        // Session must have a baseLayer or frame requests
                        // will be ignored.
                        session.updateRenderState({
                            baseLayer: glLayer
                        });
                        sessionObjects.glLayer = glLayer;
                        xr_debug(name, 'session.visibilityState=' + session.visibilityState);
                        try {
                          resolve(func(session, testDeviceController, t, sessionObjects));
                        } catch(err) {
                          reject("Test function failed with: " + err);
                        }
                      })
                      .catch((err) => {
                        xr_debug(name, 'error: ' + err);
                        reject(
                            'Session with params ' +
                            JSON.stringify(sessionMode) +
                            ' was rejected on device ' +
                            JSON.stringify(fakeDeviceInit) +
                            ' with error: ' + err);
                      });
                });
        }));
  }

  xr_promise_test(
    name + ' - webgl',
    runTest,
    properties,
    'webgl',
    {alpha: false, antialias: false, ...glcontextProperties}
  );
  xr_promise_test(
    name + ' - webgl2',
    runTest,
    properties,
    'webgl2',
    {alpha: false, antialias: false, ...glcontextProperties}
  );
}


// This function wraps the provided function in a
// simulateUserActivation() call, and resolves the promise with the
// result of func(), or an error if one is thrown
function promise_simulate_user_activation(func) {
  return new Promise((resolve, reject) => {
    navigator.xr.test.simulateUserActivation(() => {
      try { let a = func(); resolve(a); } catch(e) { reject(e); }
    });
  });
}

// This functions calls a callback with each API object as specified
// by https://immersive-web.github.io/webxr/spec/latest/, allowing
// checks to be made on all ojects.
// Arguements:
//      callback: A callback function with two arguements, the first
//                being the API object, the second being the name of
//                that API object.
function forEachWebxrObject(callback) {
  callback(window.navigator.xr, 'navigator.xr');
  callback(window.XRSession, 'XRSession');
  callback(window.XRSessionCreationOptions, 'XRSessionCreationOptions');
  callback(window.XRFrameRequestCallback, 'XRFrameRequestCallback');
  callback(window.XRPresentationContext, 'XRPresentationContext');
  callback(window.XRFrame, 'XRFrame');
  callback(window.XRLayer, 'XRLayer');
  callback(window.XRView, 'XRView');
  callback(window.XRViewport, 'XRViewport');
  callback(window.XRViewerPose, 'XRViewerPose');
  callback(window.XRWebGLLayer, 'XRWebGLLayer');
  callback(window.XRWebGLLayerInit, 'XRWebGLLayerInit');
  callback(window.XRCoordinateSystem, 'XRCoordinateSystem');
  callback(window.XRFrameOfReference, 'XRFrameOfReference');
  callback(window.XRStageBounds, 'XRStageBounds');
  callback(window.XRSessionEvent, 'XRSessionEvent');
  callback(window.XRCoordinateSystemEvent, 'XRCoordinateSystemEvent');
}

// Code for loading test API in Chromium.
async function loadChromiumResources() {
  await loadScript('/resources/chromium/webxr-test-math-helper.js');
  await import('/resources/chromium/webxr-test.js');
  await loadScript('/resources/testdriver.js');
  await loadScript('/resources/testdriver-vendor.js');

  // This infrastructure is also used by Chromium-specific internal tests that
  // may need additional resources (e.g. internal API extensions), this allows
  // those tests to rely on this infrastructure while ensuring that no tests
  // make it into public WPTs that rely on APIs outside of the webxr test API.
  if (typeof(additionalChromiumResources) !== 'undefined') {
    for (const path of additionalChromiumResources) {
      await loadScript(path);
    }
  }

  xr_debug = navigator.xr.test.Debug;
}

function setupWebKitWebXRTestAPI() {
  // WebKit setup. The internals object is used by the WebKit test runner
  // to provide JS access to internal APIs. In this case it's used to
  // ensure that XRTest is only exposed to wpt tests.
  navigator.xr.test = internals.xrTest;
  return Promise.resolve();
}
