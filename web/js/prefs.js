/* User preferences, mirroring the tray's Display settings.
 * Appearance defaults to dark and temperature to Fahrenheit, as on iOS.
 */

const KEY = "aircube.prefs";

const DEFAULTS = {
  appearance: "dark", // "dark" | "light" | "system"
  useFahrenheit: true,
};

function load() {
  try {
    return { ...DEFAULTS, ...(JSON.parse(localStorage.getItem(KEY)) || {}) };
  } catch {
    return { ...DEFAULTS };
  }
}

const state = load();
const listeners = new Set();

export const prefs = {
  get: (key) => state[key],

  set(key, value) {
    if (state[key] === value) return;
    state[key] = value;
    try {
      localStorage.setItem(KEY, JSON.stringify(state));
    } catch {
      /* private mode */
    }
    if (key === "appearance") applyAppearance();
    listeners.forEach((fn) => fn(key, value));
  },

  subscribe(fn) {
    listeners.add(fn);
    return () => listeners.delete(fn);
  },
};

export function applyAppearance() {
  const choice = state.appearance;
  const dark =
    choice === "system"
      ? window.matchMedia("(prefers-color-scheme: dark)").matches
      : choice === "dark";
  document.documentElement.dataset.theme = dark ? "dark" : "light";
}

window
  .matchMedia("(prefers-color-scheme: dark)")
  .addEventListener("change", () => {
    if (state.appearance === "system") applyAppearance();
  });
