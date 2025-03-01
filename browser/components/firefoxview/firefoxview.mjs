/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const MediaQueryDOMSorting = {
  init() {
    this.recentlyClosedTabs = document.getElementById(
      "recently-closed-tabs-container"
    );
    this.colorwayLandmark = document.getElementById("colorway-landmark");
    this.mql = window.matchMedia("(max-width: 65rem)");
    this.mql.addEventListener("change", () => this.changeHandler());
    this.changeHandler();
  },
  cleanup() {
    this.mql.removeEventListener("change", () => this.changeHandler());
  },
  changeHandler() {
    const oldFocus = document.activeElement;
    if (this.mql.matches) {
      this.recentlyClosedTabs.before(this.colorwayLandmark);
    } else {
      this.colorwayLandmark.before(this.recentlyClosedTabs);
    }
    if (oldFocus) {
      Services.focus.setFocus(oldFocus, Ci.nsIFocusManager.FLAG_NOSCROLL);
    }
  },
};

window.addEventListener("DOMContentLoaded", () => {
  Services.telemetry.setEventRecordingEnabled("firefoxview", true);
  Services.telemetry.recordEvent("firefoxview", "entered", "firefoxview", null);
  document.getElementById("recently-closed-tabs-container").onLoad();
  MediaQueryDOMSorting.init();
});

window.addEventListener("unload", () => {
  const tabPickupList = document.querySelector("tab-pickup-list");
  if (tabPickupList) {
    tabPickupList.cleanup();
  }
  document.getElementById("tab-pickup-container").cleanup();
  document.getElementById("recently-closed-tabs-container").cleanup();
  MediaQueryDOMSorting.cleanup();
});
