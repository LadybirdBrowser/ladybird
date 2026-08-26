function checkScroll() {
  // Ensure two animation frames on load to test the fallback to element
  // anchor, which gets queued for the next frame if the text fragment is not
  // found.
  requestAnimationFrame(() => {
    requestAnimationFrame(() => {
      let results = {
        scrolled: (window.pageYOffset != 0),
      };

      let key = (new URL(document.location)).searchParams.get("key");
      stashResultsThenClose(key, results);
    });
  });
}

window.addEventListener('pageshow', () => {
  if (location.hash == "#history") {
    // This is the "history" test - on the first load we'll navigate to a page
    // that calls history.back(). When we load a second time (from the back
    // navigation), record the scroll state at that point to check how history
    // scroll restoration is handled.
    if (history.state == null) {
      history.pushState("test", "test", "");
      requestAnimationFrame(() => {
        location.href = "navigate-back.html";
      });
      return;
    }
  }

  checkScroll();
});
