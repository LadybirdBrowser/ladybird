/**
 * EditorTestUtils is a helper utilities to test HTML editor.  This can be
 * instantiated per an editing host.  If you test `designMode`, the editing
 * host should be the <body> element.
 * Note that if you want to use sendKey in a sub-document, you need to include
 * testdriver.js (and related files) from the sub-document before creating this.
 */
class EditorTestUtils {
  kShift = "\uE008";
  kMeta = "\uE03d";
  kControl = "\uE009";
  kAlt = "\uE00A";

  editingHost;

  constructor(aEditingHost, aHarnessWindow = window) {
    this.editingHost = aEditingHost;
    if (aHarnessWindow != this.window && this.window.test_driver) {
      this.window.test_driver.set_test_context(aHarnessWindow);
    }
  }

  get document() {
    return this.editingHost.ownerDocument;
  }
  get window() {
    return this.document.defaultView;
  }
  get selection() {
    return this.window.getSelection();
  }

  // Return a modifier to delete per word.
  get deleteWordModifier() {
    return this.window.navigator.platform.includes("Mac") ? this.kAlt : this.kControl;
  }

  sendKey(key, modifier) {
    if (!modifier) {
      // send_keys requires element in the light DOM.
      const elementInLightDOM = (e => {
        const doc = e.ownerDocument;
        while (e.getRootNode({composed:false}) !== doc) {
          e = e.getRootNode({composed:false}).host;
        }
        return e;
      })(this.editingHost);
      return this.window.test_driver.send_keys(elementInLightDOM, key)
        .catch(() => {
          return new this.window.test_driver.Actions()
          .keyDown(key)
          .keyUp(key)
          .send();
        });
    }
    return new this.window.test_driver.Actions()
      .keyDown(modifier)
      .keyDown(key)
      .keyUp(key)
      .keyUp(modifier)
      .send();
  }

  sendDeleteKey(modifier) {
    const kDeleteKey = "\uE017";
    return this.sendKey(kDeleteKey, modifier);
  }

  sendBackspaceKey(modifier) {
    const kBackspaceKey = "\uE003";
    return this.sendKey(kBackspaceKey, modifier);
  }

  sendArrowLeftKey(modifier) {
    const kArrowLeft = "\uE012";
    return this.sendKey(kArrowLeft, modifier);
  }

  sendArrowRightKey(modifier) {
    const kArrowRight = "\uE014";
    return this.sendKey(kArrowRight, modifier);
  }

  sendMoveWordLeftKey(modifier) {
    const kArrowLeft = "\uE012";
    return this.sendKey(
      kArrowLeft,
      this.window.navigator.platform.includes("Mac")
        ? this.kAlt
        : this.kControl
    );
  }

  sendMoveWordRightKey(modifier) {
    const kArrowRight = "\uE014";
    return this.sendKey(
      kArrowRight,
      this.window.navigator.platform.includes("Mac")
        ? this.kAlt
        : this.kControl
    );
  }

  sendHomeKey(modifier) {
    const kHome = "\uE011";
    return this.sendKey(kHome, modifier);
  }

  sendEndKey(modifier) {
    const kEnd = "\uE010";
    return this.sendKey(kEnd, modifier);
  }

  sendEnterKey(modifier) {
    const kEnter = "\uE007";
    return this.sendKey(kEnter, modifier);
  }

  sendSelectAllShortcutKey() {
    return this.sendKey(
      "a",
      this.window.navigator.platform.includes("Mac")
        ? this.kMeta
        : this.kControl
    );
  }

  sendCopyShortcutKey() {
    return this.sendKey(
      "c",
      this.window.navigator.platform.includes("Mac")
        ? this.kMeta
        : this.kControl
    );
  }

  sendCutShortcutKey() {
    return this.sendKey(
      "x",
      this.window.navigator.platform.includes("Mac")
        ? this.kMeta
        : this.kControl
    );
  }

  sendPasteShortcutKey() {
    return this.sendKey(
      "v",
      this.window.navigator.platform.includes("Mac")
        ? this.kMeta
        : this.kControl
    );
  }

  sendPasteAsPlaintextShortcutKey() {
    // Ctrl/Cmd - Shift - v on Chrome and Firefox
    // Cmd - Alt - Shift - v on Safari
    const accel = this.window.navigator.platform.includes("Mac") ? this.kMeta : this.kControl;
    const isSafari = this.window.navigator.userAgent.includes("Safari");
    let actions = new this.window.test_driver.Actions();
    actions = actions.keyDown(accel).keyDown(this.kShift);
    if (isSafari) {
      actions = actions.keyDown(this.kAlt);
    }
    actions = actions.keyDown("v").keyUp("v");
    actions = actions.keyUp(accel).keyUp(this.kShift);
    if (isSafari) {
      actions = actions.keyUp(this.kAlt);
    }
    return actions.send();
  }

  // Similar to `setupDiv` in editing/include/tests.js, this method sets
  // innerHTML value of this.editingHost, and sets multiple selection ranges
  // specified with the markers.
  // - `[` specifies start boundary in a text node
  // - `{` specifies start boundary before a node
  // - `]` specifies end boundary in a text node
  // - `}` specifies end boundary after a node
  //
  // options can have following fields:
  // - selection: how to set selection, "addRange" (default),
  //              "setBaseAndExtent", "setBaseAndExtent-reverse".
  setupEditingHost(innerHTMLWithRangeMarkers, options = {}) {
    if (!options.selection) {
      options.selection = "addRange";
    }
    const startBoundaries = innerHTMLWithRangeMarkers.match(/\{|\[/g) || [];
    const endBoundaries = innerHTMLWithRangeMarkers.match(/\}|\]/g) || [];
    if (startBoundaries.length !== endBoundaries.length) {
      throw "Should match number of open/close markers";
    }

    this.editingHost.innerHTML = innerHTMLWithRangeMarkers;
    this.editingHost.focus();

    if (startBoundaries.length === 0) {
      // Don't remove the range for now since some tests may assume that
      // setting innerHTML does not remove all selection ranges.
      return;
    }

    let getNextRangeAndDeleteMarker = startNode => {
      let getNextLeafNode = node => {
        let inclusiveDeepestFirstChildNode = container => {
          while (container.firstChild) {
            container = container.firstChild;
          }
          return container;
        };
        if (node.hasChildNodes()) {
          return inclusiveDeepestFirstChildNode(node);
        }
        if (node === this.editingHost) {
          return null;
        }
        if (node.nextSibling) {
          return inclusiveDeepestFirstChildNode(node.nextSibling);
        }
        let nextSibling = (child => {
          for (
            let parent = child.parentElement;
            parent && parent != this.editingHost;
            parent = parent.parentElement
          ) {
            if (parent.nextSibling) {
              return parent.nextSibling;
            }
          }
          return null;
        })(node);
        if (!nextSibling) {
          return null;
        }
        return inclusiveDeepestFirstChildNode(nextSibling);
      };
      let scanMarkerInTextNode = (textNode, offset) => {
        return /[\{\[\]\}]/.exec(textNode.data.substr(offset));
      };
      let startMarker = ((startContainer, startOffset) => {
        let scanStartMakerInTextNode = (textNode, offset) => {
          let scanResult = scanMarkerInTextNode(textNode, offset);
          if (scanResult === null) {
            return null;
          }
          if (scanResult[0] === "}" || scanResult[0] === "]") {
            throw "An end marker is found before a start marker";
          }
          return {
            marker: scanResult[0],
            container: textNode,
            offset: scanResult.index + offset,
          };
        };
        if (startContainer.nodeType === Node.TEXT_NODE) {
          let scanResult = scanStartMakerInTextNode(
            startContainer,
            startOffset
          );
          if (scanResult !== null) {
            return scanResult;
          }
        }
        let nextNode = startContainer;
        while ((nextNode = getNextLeafNode(nextNode))) {
          if (nextNode.nodeType === Node.TEXT_NODE) {
            let scanResult = scanStartMakerInTextNode(nextNode, 0);
            if (scanResult !== null) {
              return scanResult;
            }
            continue;
          }
        }
        return null;
      })(startNode, 0);
      if (startMarker === null) {
        return null;
      }
      let endMarker = ((startContainer, startOffset) => {
        let scanEndMarkerInTextNode = (textNode, offset) => {
          let scanResult = scanMarkerInTextNode(textNode, offset);
          if (scanResult === null) {
            return null;
          }
          if (scanResult[0] === "{" || scanResult[0] === "[") {
            throw "A start marker is found before an end marker";
          }
          return {
            marker: scanResult[0],
            container: textNode,
            offset: scanResult.index + offset,
          };
        };
        if (startContainer.nodeType === Node.TEXT_NODE) {
          let scanResult = scanEndMarkerInTextNode(startContainer, startOffset);
          if (scanResult !== null) {
            return scanResult;
          }
        }
        let nextNode = startContainer;
        while ((nextNode = getNextLeafNode(nextNode))) {
          if (nextNode.nodeType === Node.TEXT_NODE) {
            let scanResult = scanEndMarkerInTextNode(nextNode, 0);
            if (scanResult !== null) {
              return scanResult;
            }
            continue;
          }
        }
        return null;
      })(startMarker.container, startMarker.offset + 1);
      if (endMarker === null) {
        throw "Found an open marker, but not found corresponding close marker";
      }
      let indexOfContainer = (container, child) => {
        let offset = 0;
        for (let node = container.firstChild; node; node = node.nextSibling) {
          if (node == child) {
            return offset;
          }
          offset++;
        }
        throw "child must be a child node of container";
      };
      let deleteFoundMarkers = () => {
        let removeNode = node => {
          let container = node.parentElement;
          let offset = indexOfContainer(container, node);
          node.remove();
          return { container, offset };
        };
        if (startMarker.container == endMarker.container) {
          // If the text node becomes empty, remove it and set collapsed range
          // to the position where there is the text node.
          if (startMarker.container.length === 2) {
            if (!/[\[\{][\]\}]/.test(startMarker.container.data)) {
              throw `Unexpected text node (data: "${startMarker.container.data}")`;
            }
            let { container, offset } = removeNode(startMarker.container);
            startMarker.container = endMarker.container = container;
            startMarker.offset = endMarker.offset = offset;
            startMarker.marker = endMarker.marker = "";
            return;
          }
          startMarker.container.data = `${startMarker.container.data.substring(
            0,
            startMarker.offset
          )}${startMarker.container.data.substring(
            startMarker.offset + 1,
            endMarker.offset
          )}${startMarker.container.data.substring(endMarker.offset + 1)}`;
          if (startMarker.offset >= startMarker.container.length) {
            startMarker.offset = endMarker.offset =
              startMarker.container.length;
            return;
          }
          endMarker.offset--; // remove the start marker's length
          if (endMarker.offset > endMarker.container.length) {
            endMarker.offset = endMarker.container.length;
          }
          return;
        }
        if (startMarker.container.length === 1) {
          let { container, offset } = removeNode(startMarker.container);
          startMarker.container = container;
          startMarker.offset = offset;
          startMarker.marker = "";
        } else {
          startMarker.container.data = `${startMarker.container.data.substring(
            0,
            startMarker.offset
          )}${startMarker.container.data.substring(startMarker.offset + 1)}`;
        }
        if (endMarker.container.length === 1) {
          let { container, offset } = removeNode(endMarker.container);
          endMarker.container = container;
          endMarker.offset = offset;
          endMarker.marker = "";
        } else {
          endMarker.container.data = `${endMarker.container.data.substring(
            0,
            endMarker.offset
          )}${endMarker.container.data.substring(endMarker.offset + 1)}`;
        }
      };
      deleteFoundMarkers();

      let handleNodeSelectMarker = () => {
        if (startMarker.marker === "{") {
          if (startMarker.offset === 0) {
            // The range start with the text node.
            let container = startMarker.container.parentElement;
            startMarker.offset = indexOfContainer(
              container,
              startMarker.container
            );
            startMarker.container = container;
          } else if (startMarker.offset === startMarker.container.data.length) {
            // The range start after the text node.
            let container = startMarker.container.parentElement;
            startMarker.offset =
              indexOfContainer(container, startMarker.container) + 1;
            startMarker.container = container;
          } else {
            throw 'Start marker "{" is allowed start or end of a text node';
          }
        }
        if (endMarker.marker === "}") {
          if (endMarker.offset === 0) {
            // The range ends before the text node.
            let container = endMarker.container.parentElement;
            endMarker.offset = indexOfContainer(container, endMarker.container);
            endMarker.container = container;
          } else if (endMarker.offset === endMarker.container.data.length) {
            // The range ends with the text node.
            let container = endMarker.container.parentElement;
            endMarker.offset =
              indexOfContainer(container, endMarker.container) + 1;
            endMarker.container = container;
          } else {
            throw 'End marker "}" is allowed start or end of a text node';
          }
        }
      };
      handleNodeSelectMarker();

      let range = document.createRange();
      range.setStart(startMarker.container, startMarker.offset);
      range.setEnd(endMarker.container, endMarker.offset);
      return range;
    };

    let ranges = [];
    for (
      let range = getNextRangeAndDeleteMarker(this.editingHost.firstChild);
      range;
      range = getNextRangeAndDeleteMarker(range.endContainer)
    ) {
      ranges.push(range);
    }

    if (options.selection != "addRange" && ranges.length > 1) {
      throw `Failed due to invalid selection option, ${options.selection}, for multiple selection ranges`;
    }

    this.selection.removeAllRanges();
    for (const range of ranges) {
      if (options.selection == "addRange") {
        this.selection.addRange(range);
      } else if (options.selection == "setBaseAndExtent") {
        this.selection.setBaseAndExtent(
          range.startContainer,
          range.startOffset,
          range.endContainer,
          range.endOffset
        );
      } else if (options.selection == "setBaseAndExtent-reverse") {
        this.selection.setBaseAndExtent(
          range.endContainer,
          range.endOffset,
          range.startContainer,
          range.startOffset
        );
      } else {
        throw `Failed due to invalid selection option, ${options.selection}`;
      }
    }

    if (this.selection.rangeCount != ranges.length) {
      throw `Failed to set selection to the given ranges whose length is ${ranges.length}, but only ${this.selection.rangeCount} ranges are added`;
    }
  }

  // Originated from normalizeSerializedStyle in include/tests.js
  normalizeStyleAttributeValues() {
    for (const element of Array.from(
      this.editingHost.querySelectorAll("[style]")
    )) {
      element.setAttribute(
        "style",
        element
          .getAttribute("style")
          // Random spacing differences
          .replace(/; ?$/, "")
          .replace(/: /g, ":")
          // Gecko likes "transparent"
          .replace(/transparent/g, "rgba(0, 0, 0, 0)")
          // WebKit likes to look overly precise
          .replace(/, 0.496094\)/g, ", 0.5)")
          // Gecko converts anything with full alpha to "transparent" which
          // then becomes "rgba(0, 0, 0, 0)", so we have to make other
          // browsers match
          .replace(/rgba\([0-9]+, [0-9]+, [0-9]+, 0\)/g, "rgba(0, 0, 0, 0)")
      );
    }
  }

  static getRangeArrayDescription(arrayOfRanges) {
    if (arrayOfRanges === null) {
      return "null";
    }
    if (arrayOfRanges === undefined) {
      return "undefined";
    }
    if (!Array.isArray(arrayOfRanges)) {
      return "Unknown Object";
    }
    if (arrayOfRanges.length === 0) {
      return "[]";
    }
    let result = "";
    for (let range of arrayOfRanges) {
      if (result === "") {
        result = "[";
      } else {
        result += ",";
      }
      result += `{${EditorTestUtils.getRangeDescription(range)}}`;
    }
    result += "]";
    return result;
  }

  static getNodeDescription(node) {
    if (!node) {
      return "null";
    }
    switch (node.nodeType) {
      case Node.TEXT_NODE:
      case Node.COMMENT_NODE:
      case Node.CDATA_SECTION_NODE:
        return `${node.nodeName} "${node.data.replaceAll("\n", "\\\\n")}"`;
      case Node.ELEMENT_NODE:
        return `<${node.nodeName.toLowerCase()}${
            node.hasAttribute("id") ? ` id="${node.getAttribute("id")}"` : ""
          }${
            node.hasAttribute("class") ? ` class="${node.getAttribute("class")}"` : ""
          }${
            node.hasAttribute("contenteditable")
              ? ` contenteditable="${node.getAttribute("contenteditable")}"`
              : ""
          }${
            node.inert ? ` inert` : ""
          }${
            node.hidden ? ` hidden` : ""
          }${
            node.readonly ? ` readonly` : ""
          }${
            node.disabled ? ` disabled` : ""
          }>`;
      default:
        return `${node.nodeName}`;
    }
  }

  static getRangeDescription(range) {
    if (range === null) {
      return "null";
    }
    if (range === undefined) {
      return "undefined";
    }
    return range.startContainer == range.endContainer &&
      range.startOffset == range.endOffset
      ? `(${EditorTestUtils.getNodeDescription(range.startContainer)}, ${range.startOffset})`
      : `(${EditorTestUtils.getNodeDescription(range.startContainer)}, ${
          range.startOffset
        }) - (${EditorTestUtils.getNodeDescription(range.endContainer)}, ${range.endOffset})`;
  }

  static waitForRender() {
    return new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  }

}
