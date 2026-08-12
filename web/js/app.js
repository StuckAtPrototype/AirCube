/* App shell: hash router, device registry wiring, appearance boot.
 *
 * Unlike the phone and tray apps, which always land on the device list, a
 * single connected cube goes straight to its detail page. Back stays available
 * so the list is never out of reach, and a second cube appearing does not pull
 * the user off the page they are on.
 */

import { h, toast } from "./ui.js";
import { applyAppearance } from "./prefs.js";
import { DeviceRegistry } from "./devices.js";
import { HomeView } from "./views/home.js";
import { DetailView } from "./views/detail.js";
import { SettingsView } from "./views/settings.js";
import { openFlashModal } from "./views/flash-modal.js";

const STALE_TICK_MS = 2000;

class App {
  constructor() {
    this.registry = new DeviceRegistry();
    this.route = { name: "home" };
    this.suppressAutoRoute = false;
    this.lastDeviceCount = 0;

    this.home = new HomeView(this.registry, {
      onOpenDetail: (id) => this.navigate(`#/device/${id}`),
      onOpenSettings: () => this.navigate("#/settings"),
      onConnect: () => this.connect(),
    });
    this.detail = new DetailView(this.registry, {
      onBack: () => {
        this.suppressAutoRoute = true;
        this.navigate("#/");
      },
      onFlash: (device) => this.openFlash(device),
    });
    this.settings = new SettingsView(this.registry, {
      onBack: () => history.back(),
      onFlash: (device) => this.openFlash(device),
    });

    this.routes = {
      home: h("div.route", this.home.el),
      device: h("div.route", this.detail.el),
      settings: h("div.route", this.settings.el),
    };

    this.banner = h("div", { style: { display: "none" } });
    document.querySelector("#app").append(
      this.banner,
      this.routes.home,
      this.routes.device,
      this.routes.settings,
    );
  }

  async start() {
    applyAppearance();

    if (!this.registry.supported) {
      this.showBanner(
        "This browser cannot talk to USB devices. Web Serial is available in Chrome, Edge and other Chromium browsers on desktop.",
        "warn",
      );
      this.render();
      return;
    }

    window.addEventListener("hashchange", () => this.render());
    this.registry.addEventListener("change", () => this.onRegistryChange());

    try {
      await this.registry.init();
    } catch (err) {
      this.showBanner(`Could not list serial ports: ${err.message}`, "warn");
    }

    this.onRegistryChange();
    this.render();
    setInterval(() => this.refreshActive(), STALE_TICK_MS);
  }

  showBanner(message, kind = "") {
    this.banner.replaceChildren(
      h(`div.banner${kind ? `.${kind}` : ""}`, { text: message }),
    );
    this.banner.style.display = "";
  }

  async connect() {
    try {
      await this.registry.requestPort();
    } catch (err) {
      // The user dismissing the browser's port chooser is not an error.
      if (err?.name !== "NotFoundError" && err?.name !== "AbortError") {
        toast(err.message || String(err), "err");
      }
    }
  }

  openFlash(device) {
    openFlashModal(this.registry, device).catch((err) => toast(err.message, "err"));
  }

  onRegistryChange() {
    const count = this.registry.devices.length;
    if (count !== this.lastDeviceCount) {
      this.lastDeviceCount = count;
      this.suppressAutoRoute = false;
    }

    if (
      count === 1 &&
      !this.suppressAutoRoute &&
      (location.hash === "" || location.hash === "#/" || location.hash === "#")
    ) {
      location.replace(`#/device/${this.registry.devices[0].id}`);
      return;
    }

    this.render();
  }

  navigate(hash) {
    if (location.hash === hash) this.render();
    else location.hash = hash;
  }

  parseHash() {
    const hash = location.hash.replace(/^#\/?/, "");
    if (hash.startsWith("device/")) return { name: "device", id: hash.slice(7) };
    if (hash === "settings") return { name: "settings" };
    return { name: "home" };
  }

  render() {
    const route = this.parseHash();

    if (route.name === "device") {
      const device = this.registry.byId(route.id);
      if (!device) {
        // The cube was unplugged while its page was open.
        location.replace("#/");
        return;
      }
      if (this.detail.device !== device) this.detail.setDevice(device);
    }

    this.route = route;
    for (const [name, node] of Object.entries(this.routes)) {
      node.classList.toggle("active", name === route.name);
    }
    this.refreshActive();
  }

  refreshActive() {
    switch (this.route.name) {
      case "device":
        if (this.registry.byId(this.route.id)) this.detail.refresh();
        break;
      case "settings":
        this.settings.refresh();
        break;
      default:
        this.home.refresh();
    }
  }
}

const app = new App();
app.start();
