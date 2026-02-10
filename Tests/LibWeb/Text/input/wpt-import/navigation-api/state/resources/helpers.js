window.updateStateBasedOnTestVariant = (w, state) => {
  const usp = new URLSearchParams(location.search);
  const method = usp.get("method");

  switch (method) {
    case "navigate": {
      w.navigation.navigate("#", { history: "replace", state });
      break;
    }
    case "updateCurrentEntry": {
      w.navigation.updateCurrentEntry({ state });
      break;
    }
    default: {
      assert_unreached(`method must be either "navigate" or "updateCurrentEntry"`);
    }
  }
};
