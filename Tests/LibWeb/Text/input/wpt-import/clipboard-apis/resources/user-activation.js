'use strict';

// In order to use this function, please import testdriver.js and
// testdriver-vendor.js, and include a <body> element.
async function waitForUserActivation() {
  if (window.opener) {
    throw new Error(
        "waitForUserActivation() only works in the top-level frame");
  }
  const loadedPromise = new Promise(resolve => {
    if(document.readyState == 'complete') {
      resolve();
      return;
    }
    window.addEventListener('load', resolve, {once: true});
  });
  await loadedPromise;

  const clickedPromise = new Promise(resolve => {
    document.body.addEventListener('click', resolve, {once: true});
  });

  test_driver.click(document.body);
  await clickedPromise;
}

async function trySetPermission(perm, state) {
  try {
    await test_driver.set_permission({ name: perm }, state)
  } catch {
    // This is expected, as clipboard permissions are not supported by every engine
    // and also the set_permission. The permission is not required by such engines as
    // they require user activation instead.
  }
}

async function tryGrantReadPermission() {
  await trySetPermission("clipboard-read", "granted");
}

async function tryGrantWritePermission() {
  await trySetPermission("clipboard-write", "granted");
}

