// License storage and rules. Runs inside a SQLite-backed Durable Object (one strongly
// consistent database), and in Node tests through a tiny adapter with the same exec() API.

const ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no 0/O/1/I confusion

export function newKey() {
  const bytes = crypto.getRandomValues(new Uint8Array(20));
  let out = "";
  for (const b of bytes) out += ALPHABET[b & 31];
  // LUNAR-XXXXX-XXXXX-XXXXX-XXXXX  (100 bits of randomness)
  return "LUNAR-" + out.match(/.{5}/g).join("-");
}

export function normalizeKey(key) {
  return String(key || "").toUpperCase().replace(/[^A-Z0-9]/g, "").replace(/^LUNAR/, "");
}

function canonical(key) {
  const k = normalizeKey(key);
  return k.length === 20 ? "LUNAR-" + k.match(/.{5}/g).join("-") : null;
}

export class Store {
  constructor(sql, now = () => Date.now()) {
    this.sql = sql;
    this.now = now;
    sql.exec(`CREATE TABLE IF NOT EXISTS licenses (
      key TEXT PRIMARY KEY, email TEXT, session TEXT UNIQUE, payment TEXT,
      created INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0, max_devices INTEGER NOT NULL DEFAULT 2, note TEXT)`);
    sql.exec(`CREATE TABLE IF NOT EXISTS activations (
      key TEXT NOT NULL, device TEXT NOT NULL, label TEXT, created INTEGER NOT NULL, seen INTEGER NOT NULL,
      PRIMARY KEY (key, device))`);
  }
  rows(query, ...args) { return this.sql.exec(query, ...args).toArray(); }
  license(key) { const k = canonical(key); return k ? this.rows("SELECT * FROM licenses WHERE key = ?", k)[0] || null : null; }

  // Idempotent: one Stripe checkout session always maps to the same key.
  issue({ session = null, payment = null, email = "", maxDevices = 2, note = "" } = {}) {
    if (session) {
      const existing = this.rows("SELECT * FROM licenses WHERE session = ?", session)[0];
      if (existing) return existing;
    }
    for (let i = 0; i < 5; i++) {
      const key = newKey();
      if (this.rows("SELECT key FROM licenses WHERE key = ?", key).length) continue;
      this.sql.exec("INSERT INTO licenses (key, email, session, payment, created, max_devices, note) VALUES (?, ?, ?, ?, ?, ?, ?)",
        key, email, session, payment, this.now(), maxDevices, note);
      return { ...this.license(key), fresh: true };
    }
    throw new Error("could not allocate key");
  }

  activate({ key, device, label = "" }) {
    const lic = this.license(key);
    if (!lic) return { ok: false, code: "not_found" };
    if (lic.revoked) return { ok: false, code: "revoked" };
    if (!/^[a-f0-9]{16,128}$/.test(String(device || ""))) return { ok: false, code: "bad_device" };
    const known = this.rows("SELECT * FROM activations WHERE key = ? AND device = ?", lic.key, device)[0];
    if (!known) {
      const used = this.rows("SELECT COUNT(*) AS n FROM activations WHERE key = ?", lic.key)[0].n;
      if (used >= lic.max_devices) return { ok: false, code: "device_limit", limit: lic.max_devices };
      this.sql.exec("INSERT INTO activations (key, device, label, created, seen) VALUES (?, ?, ?, ?, ?)",
        lic.key, device, String(label).slice(0, 64), this.now(), this.now());
    } else {
      this.sql.exec("UPDATE activations SET seen = ?, label = ? WHERE key = ? AND device = ?", this.now(), String(label).slice(0, 64) || known.label, lic.key, device);
    }
    return { ok: true, key: lic.key };
  }

  validate({ key, device }) {
    const lic = this.license(key);
    if (!lic) return { ok: false, code: "not_found" };
    if (lic.revoked) return { ok: false, code: "revoked" };
    const known = this.rows("SELECT * FROM activations WHERE key = ? AND device = ?", lic.key, device)[0];
    if (!known) return { ok: false, code: "not_activated" };
    this.sql.exec("UPDATE activations SET seen = ? WHERE key = ? AND device = ?", this.now(), lic.key, device);
    return { ok: true, key: lic.key };
  }

  deactivate({ key, device }) {
    const lic = this.license(key);
    if (!lic) return { ok: false, code: "not_found" };
    this.sql.exec("DELETE FROM activations WHERE key = ? AND device = ?", lic.key, device);
    return { ok: true };
  }

  // ---- owner tools ----
  revoke(key, revoked = true) { const lic = this.license(key); if (!lic) return false; this.sql.exec("UPDATE licenses SET revoked = ? WHERE key = ?", revoked ? 1 : 0, lic.key); return true; }
  revokePayment(payment) { this.sql.exec("UPDATE licenses SET revoked = 1 WHERE payment = ?", payment); }
  reset(key) { const lic = this.license(key); if (!lic) return false; this.sql.exec("DELETE FROM activations WHERE key = ?", lic.key); return true; }
  setDevices(key, n) { const lic = this.license(key); if (!lic) return false; this.sql.exec("UPDATE licenses SET max_devices = ? WHERE key = ?", n, lic.key); return true; }
  find(query) {
    const q = String(query || "").trim();
    const lic = this.license(q);
    const list = lic ? [lic] : this.rows("SELECT * FROM licenses WHERE email = ? OR session = ? OR payment = ? ORDER BY created DESC LIMIT 50", q.toLowerCase(), q, q);
    return list.map((l) => ({ ...l, devices: this.rows("SELECT device, label, created, seen FROM activations WHERE key = ?", l.key) }));
  }
  recent(limit = 50) { return this.rows("SELECT * FROM licenses ORDER BY created DESC LIMIT ?", limit); }
  stats() {
    return {
      licenses: this.rows("SELECT COUNT(*) AS n FROM licenses")[0].n,
      active: this.rows("SELECT COUNT(*) AS n FROM licenses WHERE revoked = 0")[0].n,
      devices: this.rows("SELECT COUNT(*) AS n FROM activations")[0].n,
    };
  }
}
