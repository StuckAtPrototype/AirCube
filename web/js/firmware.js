/* Firmware catalog.
 *
 * Release binaries are served from this site rather than pulled from GitHub
 * Releases at runtime: release assets redirect to
 * release-assets.githubusercontent.com, which sends no Access-Control-Allow-Origin
 * header, so a browser fetch of them is blocked. The Pages workflow copies the
 * newest assets into firmware/ and writes manifest.json alongside them.
 */

const MANIFEST_URL = "./firmware/manifest.json";

export async function loadManifest() {
  try {
    const response = await fetch(MANIFEST_URL, { cache: "no-cache" });
    if (!response.ok) return { releases: [] };
    const manifest = await response.json();
    return { latest: manifest.latest, releases: manifest.releases || [] };
  } catch {
    // Running from a checkout without the CI-generated manifest is fine;
    // the user can still pick a local .bin file.
    return { releases: [] };
  }
}

async function sha256Hex(buffer) {
  const digest = await crypto.subtle.digest("SHA-256", buffer);
  return [...new Uint8Array(digest)]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

/** Download one catalog entry and verify it against the manifest digest. */
export async function fetchImage(entry, onProgress) {
  const response = await fetch(`./firmware/${entry.file}`);
  if (!response.ok) {
    throw new Error(`Could not download ${entry.file} (HTTP ${response.status})`);
  }

  const total = Number(response.headers.get("content-length")) || entry.size || 0;
  const chunks = [];
  let received = 0;
  const reader = response.body.getReader();
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
    received += value.length;
    if (total) onProgress?.(received / total);
  }

  const bytes = new Uint8Array(received);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.length;
  }

  if (entry.sha256) {
    const actual = await sha256Hex(bytes);
    if (actual !== entry.sha256.toLowerCase()) {
      throw new Error(
        `${entry.file} failed its checksum. Reload the page and try again.`,
      );
    }
  }
  return bytes;
}

export async function readLocalFile(file) {
  return new Uint8Array(await file.arrayBuffer());
}

/**
 * Basic sanity check on a user-supplied image: ESP-IDF images start with the
 * 0xE9 magic byte, and a merged release image has the bootloader at offset 0.
 */
export function looksLikeEspImage(bytes) {
  return bytes.length > 0 && bytes[0] === 0xe9;
}

export const formatSize = (bytes) => `${(bytes / 1024).toFixed(0)} KB`;
