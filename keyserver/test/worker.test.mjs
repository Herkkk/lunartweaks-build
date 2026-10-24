// End-to-end tests of the Worker with a real SQLite database, real WebCrypto and a fake Stripe API.
import assert from "node:assert/strict";
import { DatabaseSync } from "node:sqlite";
import worker, { Licenses } from "../src/worker.js";
import { responseMessage } from "../src/crypto.js";
import { Store, normalizeKey } from "../src/store.js";

// SQLite adapter with the Durable Object `sql.exec(query, ...args).toArray()` shape.
function sqlAdapter() {
  const db = new DatabaseSync(":memory:");
  return { exec(q, ...args) { const st = db.prepare(q); const isRead = /^\s*select/i.test(q); const rows = isRead ? st.all(...args) : (st.run(...args), []); return { toArray: () => rows.map((r) => ({ ...r })) }; } };
}
const instance = new Licenses({ storage: { sql: sqlAdapter() } }, {});
const ns = { idFromName: () => "main", get: () => ({ call: async (m, a) => instance.call(m, a), allow: async (ip, l) => instance.allow(ip, l) }) };

const pair = await crypto.subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, true, ["sign", "verify"]);
const env = {
  LICENSES: ns, SITE_URL: "https://lunartweaks.se", MAX_DEVICES: "2", STRIPE_SECRET_KEY: "sk_test_x", STRIPE_PRICE_ID: "price_1",
  STRIPE_WEBHOOK_SECRET: "whsec_test", ADMIN_TOKEN: "admin-secret", SIGNING_KEY: JSON.stringify(await crypto.subtle.exportKey("jwk", pair.privateKey)),
};
// Fake Stripe.
const sessions = { cs_paid_1: { id: "cs_paid_1", payment_status: "paid", payment_intent: "pi_1", customer_details: { email: "Buyer@Example.com" }, metadata: { product: "lunartweaks-pro" } },
  cs_unpaid: { id: "cs_unpaid", payment_status: "unpaid", metadata: { product: "lunartweaks-pro" } } };
let created = null;
globalThis.fetch = async (url, init = {}) => {
  url = String(url);
  if (url === "https://api.stripe.com/v1/checkout/sessions" && init.method === "POST") { created = new URLSearchParams(init.body); return new Response(JSON.stringify({ id: "cs_new", url: "https://checkout.stripe.com/c/pay/cs_new" })); }
  const m = url.match(/checkout\/sessions\/(.+)$/);
  if (m) { const s = sessions[decodeURIComponent(m[1])]; return new Response(JSON.stringify(s || { error: { message: "No such session" } }), { status: s ? 200 : 404 }); }
  throw new Error("unexpected fetch " + url);
};
const req = (path, init) => worker.fetch(new Request("https://keys.lunartweaks.se" + path, init), env);
const post = (path, body, headers = {}) => req(path, { method: "POST", body: typeof body === "string" ? body : JSON.stringify(body), headers: { "content-type": "application/json", ...headers } });
const verify = async (purpose, nonce, device, body) => {
  const sig = Uint8Array.from(body.sig.match(/../g).map((h) => parseInt(h, 16)));
  return crypto.subtle.verify({ name: "ECDSA", hash: "SHA-256" }, pair.publicKey, sig, new TextEncoder().encode(responseMessage(purpose, nonce, device, body.status, body.ts)));
};

// 1. /buy creates a one-time checkout for the Pro price and redirects to Stripe.
let r = await req("/buy");
assert.equal(r.status, 303); assert.equal(r.headers.get("location"), "https://checkout.stripe.com/c/pay/cs_new");
assert.equal(created.get("mode"), "payment"); assert.equal(created.get("line_items[0][price]"), "price_1");
assert.ok(created.get("success_url").includes("{CHECKOUT_SESSION_ID}"));

// 2. Success page issues exactly one key per paid session (idempotent) and refuses unpaid ones.
r = await req("/success?session_id=cs_paid_1"); assert.equal(r.status, 200);
const page = await r.text(); const key = page.match(/LUNAR-[A-Z0-9-]{23}/)[0];
r = await req("/success?session_id=cs_paid_1"); assert.ok((await r.text()).includes(key), "same key on reload");
r = await req("/success?session_id=cs_unpaid"); assert.equal(r.status, 202);

// 3. Webhook: signature checked; duplicate events don't create new keys.
const event = JSON.stringify({ type: "checkout.session.completed", data: { object: sessions.cs_paid_1 } });
const t = Math.floor(Date.now() / 1000);
const hmacKey = await crypto.subtle.importKey("raw", new TextEncoder().encode("whsec_test"), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
const v1 = [...new Uint8Array(await crypto.subtle.sign("HMAC", hmacKey, new TextEncoder().encode(`${t}.${event}`)))].map((b) => b.toString(16).padStart(2, "0")).join("");
r = await post("/stripe/webhook", event, { "stripe-signature": `t=${t},v1=${v1}` }); assert.equal(r.status, 200);
r = await post("/stripe/webhook", event, { "stripe-signature": `t=${t},v1=${"0".repeat(64)}` }); assert.equal(r.status, 400);
assert.equal(instance.call("stats", []).licenses, 1);

// 4. App: activate on 2 PCs, 3rd refused; answers are signed and bound to nonce + device.
const devA = "a".repeat(40), devB = "b".repeat(40), devC = "c".repeat(40), nonce = "1f".repeat(16);
r = await post("/v1/activate", { key: key.toLowerCase().replace(/-/g, " "), device: devA, label: "PC-A", nonce });
let body = await r.json(); assert.equal(body.ok, true); assert.equal(body.status, "granted"); assert.ok(await verify("activate", nonce, devA, body));
assert.equal(await verify("activate", "2e".repeat(16), devA, body), false, "nonce binding");
assert.equal((await (await post("/v1/activate", { key, device: devB, nonce })).json()).ok, true);
body = await (await post("/v1/activate", { key, device: devC, nonce })).json(); assert.equal(body.status, "device_limit"); assert.ok(await verify("activate", nonce, devC, body));
assert.equal((await (await post("/v1/validate", { key, device: devA, nonce })).json()).status, "granted");
assert.equal((await (await post("/v1/validate", { key, device: devC, nonce })).json()).status, "not_activated");
assert.equal((await (await post("/v1/validate", { key: "LUNAR-AAAAA-AAAAA-AAAAA-AAAAA", device: devA, nonce })).json()).status, "not_found");

// 5. Moving to a new PC: deactivate B, then C works.
await post("/v1/deactivate", { key, device: devB });
assert.equal((await (await post("/v1/activate", { key, device: devC, nonce })).json()).ok, true);

// 6. Refund revokes the key.
const refund = JSON.stringify({ type: "charge.refunded", data: { object: { payment_intent: "pi_1" } } });
const v2 = [...new Uint8Array(await crypto.subtle.sign("HMAC", hmacKey, new TextEncoder().encode(`${t}.${refund}`)))].map((b) => b.toString(16).padStart(2, "0")).join("");
await post("/stripe/webhook", refund, { "stripe-signature": `t=${t},v1=${v2}` });
assert.equal((await (await post("/v1/validate", { key, device: devA, nonce })).json()).status, "revoked");

// 7. Admin: token required; manual key, restore, reset.
assert.equal((await req("/admin/api/stats")).status, 401);
const auth = { authorization: "Bearer admin-secret" };
const manual = await (await post("/admin/api/issue", { email: "friend@x.se" }, auth)).json(); assert.match(manual.key, /^LUNAR-/);
await post("/admin/api/revoke", { key, revoked: false }, auth);
assert.equal((await (await post("/v1/validate", { key, device: devA, nonce })).json()).status, "granted");
await post("/admin/api/reset", { key }, auth);
assert.equal((await (await post("/v1/validate", { key, device: devA, nonce })).json()).status, "not_activated");
const found = await (await req("/admin/api/find?q=buyer@example.com", { headers: auth })).json(); assert.equal(found[0].key, key);

// 8. Keys: format and normalisation.
assert.equal(normalizeKey(" lunar-abcde-fghjk-lmnpq-rstuv "), "ABCDEFGHJKLMNPQRSTUV");
const s = new Store(sqlAdapter()); const ks = new Set(); for (let i = 0; i < 300; i++) ks.add(s.issue({}).key); assert.equal(ks.size, 300);

// 9. Rate limit on app endpoints.
let limited = false; for (let i = 0; i < 60; i++) { const x = await post("/v1/validate", { key, device: devA, nonce }); if (x.status === 429) limited = true; }
assert.ok(limited);
console.log("PASS: checkout, success page, webhooks (issue/refund/signature), signed activation, device limit + moving PCs, admin, rate limit");
