/* Small DOM helpers and the shared chrome: pills, toasts, menus, modals. */

/**
 * Create an element.
 * @param {string} spec tag with optional .classes, e.g. "div.card.hero"
 * @param {object|string|Node|Array} [props] attributes, or children directly
 * @param {...(string|Node)} children
 */
export function h(spec, props, ...children) {
  const [tag, ...classes] = spec.split(".");
  const node = document.createElement(tag || "div");
  if (classes.length) node.className = classes.join(" ");

  if (props != null && (typeof props === "string" || props instanceof Node || Array.isArray(props))) {
    children.unshift(props);
  } else if (props) {
    for (const [key, value] of Object.entries(props)) {
      if (value == null || value === false) continue;
      if (key === "class") node.className = [node.className, value].filter(Boolean).join(" ");
      else if (key === "text") node.textContent = value;
      else if (key === "html") node.innerHTML = value;
      else if (key === "style") Object.assign(node.style, value);
      else if (key.startsWith("on")) node.addEventListener(key.slice(2).toLowerCase(), value);
      else if (key === "dataset") Object.assign(node.dataset, value);
      else node.setAttribute(key, value === true ? "" : value);
    }
  }

  for (const child of children.flat(Infinity)) {
    if (child == null || child === false) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

export const clear = (node) => {
  while (node.firstChild) node.firstChild.remove();
  return node;
};

/** Status pill; pass a quality index to tint it, or null to stay neutral. */
export function pill(text, qualityClass) {
  const node = h("span.pill", { text });
  setPill(node, text, qualityClass);
  return node;
}

export function setPill(node, text, qualityClass) {
  node.textContent = text;
  node.className = qualityClass ? `pill tinted ${qualityClass}` : "pill";
}

export function statusDot(qualityClass) {
  return h("span", { class: qualityClass ? `dot tinted ${qualityClass}` : "dot" });
}

/** Segmented capsule control, matching PillPicker in aircubeapp/ui/widgets.py. */
export function pillPicker(options, selected, onSelect) {
  const node = h("div.pill-picker", { role: "tablist" });
  const buttons = options.map((option) =>
    h("button", {
      type: "button",
      role: "tab",
      text: option.label,
      "aria-selected": String(option.value === selected),
      onclick: () => {
        select(option.value);
        onSelect(option.value);
      },
    }),
  );
  node.append(...buttons);

  function select(value) {
    options.forEach((option, index) => {
      buttons[index].setAttribute("aria-selected", String(option.value === value));
    });
  }
  node.select = select;
  return node;
}

export function toggle(checked, onChange, { disabled = false } = {}) {
  const node = h("button.toggle", {
    type: "button",
    role: "switch",
    "aria-checked": String(Boolean(checked)),
    disabled,
    onclick: () => {
      const next = node.getAttribute("aria-checked") !== "true";
      node.setAttribute("aria-checked", String(next));
      onChange(next);
    },
  });
  return node;
}

/** Range input whose filled track follows the value. */
export function slider({ min = 0, max = 100, value = 0, disabled = false, onInput, onCommit }) {
  const node = h("input", { type: "range", min, max, value, disabled });
  const paint = () => {
    const fraction = (node.value - min) / (max - min || 1);
    node.style.setProperty("--fill", `${fraction * 100}%`);
  };
  node.addEventListener("input", () => {
    paint();
    onInput?.(Number(node.value));
  });
  node.addEventListener("change", () => onCommit?.(Number(node.value)));
  node.setValue = (v) => {
    node.value = v;
    paint();
  };
  paint();
  return node;
}

export function progressBar() {
  const fill = h("span");
  const node = h("div.progress", fill);
  node.setFraction = (fraction) => {
    node.classList.remove("indeterminate");
    fill.style.width = `${Math.max(0, Math.min(1, fraction)) * 100}%`;
  };
  node.setIndeterminate = () => {
    node.classList.add("indeterminate");
    fill.style.width = "";
  };
  return node;
}

// ------------------------------------------------------------------- overlays

let toastHost;

export function toast(message, kind = "info") {
  if (!toastHost) {
    toastHost = h("div.toasts");
    document.body.append(toastHost);
  }
  const node = h(`div.toast${kind === "err" ? ".err" : ""}`, { text: message });
  toastHost.append(node);
  setTimeout(() => node.remove(), kind === "err" ? 6000 : 3200);
}

/** Anchored popup menu. Items are {label, onSelect} or "-" for a separator. */
export function openMenu(anchor, items) {
  document.querySelector(".menu")?.remove();
  const node = h("div.menu");
  for (const item of items) {
    if (item === "-") {
      node.append(h("hr"));
      continue;
    }
    node.append(
      h("button", {
        type: "button",
        text: item.label,
        onclick: () => {
          node.remove();
          item.onSelect();
        },
      }),
    );
  }
  document.body.append(node);

  const rect = anchor.getBoundingClientRect();
  const width = node.offsetWidth;
  node.style.top = `${rect.bottom + window.scrollY + 4}px`;
  node.style.left = `${Math.max(8, Math.min(rect.right - width, window.innerWidth - width - 8)) + window.scrollX}px`;

  const dismiss = (event) => {
    if (!node.contains(event.target)) {
      node.remove();
      document.removeEventListener("pointerdown", dismiss);
      document.removeEventListener("keydown", onKey);
    }
  };
  const onKey = (event) => {
    if (event.key === "Escape") {
      node.remove();
      document.removeEventListener("pointerdown", dismiss);
      document.removeEventListener("keydown", onKey);
    }
  };
  setTimeout(() => {
    document.addEventListener("pointerdown", dismiss);
    document.addEventListener("keydown", onKey);
  });
}

/** Modal dialog. Returns {backdrop, body, close}. */
export function openModal(title, { closable = true } = {}) {
  const body = h("div", { style: { display: "contents" } });
  const modal = h("div.modal", h("h2", { text: title }), body);
  const backdrop = h("div.modal-backdrop", modal);

  const close = () => {
    backdrop.remove();
    document.removeEventListener("keydown", onKey);
  };
  const onKey = (event) => {
    if (event.key === "Escape" && backdrop.dataset.locked !== "true") close();
  };

  backdrop.addEventListener("pointerdown", (event) => {
    if (event.target === backdrop && closable && backdrop.dataset.locked !== "true") close();
  });
  document.addEventListener("keydown", onKey);
  document.body.append(backdrop);
  return { backdrop, body, close };
}

export function confirmDialog(title, message, confirmLabel = "Confirm") {
  return new Promise((resolve) => {
    const { body, close } = openModal(title);
    body.append(
      h("p.muted", { text: message, style: { margin: "0", lineHeight: "1.6" } }),
      h(
        "div.actions",
        h("button.btn", {
          type: "button",
          text: "Cancel",
          onclick: () => {
            close();
            resolve(false);
          },
        }),
        h("button.btn.primary", {
          type: "button",
          text: confirmLabel,
          onclick: () => {
            close();
            resolve(true);
          },
        }),
      ),
    );
  });
}

export function promptDialog(title, label, initialValue = "") {
  return new Promise((resolve) => {
    const { body, close } = openModal(title);
    const input = h("input", { type: "text", value: initialValue, style: { flex: "1" } });
    const submit = () => {
      close();
      resolve(input.value.trim() || null);
    };
    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") submit();
    });
    body.append(
      h("div.field", h("label", { text: label }), input),
      h(
        "div.actions",
        h("button.btn", {
          type: "button",
          text: "Cancel",
          onclick: () => {
            close();
            resolve(null);
          },
        }),
        h("button.btn.primary", { type: "button", text: "Save", onclick: submit }),
      ),
    );
    setTimeout(() => input.focus());
  });
}

export function downloadFile(filename, text, mime = "text/csv") {
  const url = URL.createObjectURL(new Blob([text], { type: mime }));
  const link = h("a", { href: url, download: filename });
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
