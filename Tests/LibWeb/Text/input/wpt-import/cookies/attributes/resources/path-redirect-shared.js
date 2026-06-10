// Note: this function has a dependency on testdriver.js. Any test files calling
// it should include testdriver.js and testdriver-vendor.js
window.addEventListener("message", (e) => {
  setTestContextUsingRootWindow();
  if (e.data == "getAndExpireCookiesForRedirectTest") {
    const cookies = document.cookie;
    test_driver.delete_all_cookies().then(() => {
      e.source.postMessage({"cookies": cookies}, '*');
    });
  }
});
