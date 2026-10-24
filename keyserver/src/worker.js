// LunarTweaks key server — Cloudflare Worker + SQLite Durable Object.
//
//   GET  /buy                 → creates a Stripe Checkout session (one-time Pro) and redirects to it
//   GET  /success?session_id  → confirms payment with Stripe and shows the buyer's key
//   POST /stripe/webhook      → issues keys for completed checkouts, revokes refunded/disputed ones
//   POST /v1/activate|validate|deactivate → used by the desktop app; answers are ECDSA-signed
//   GET  /admin               → owner panel (find, revoke, reset devices, issue keys manually)
//
// Secrets (wrangler secret put / GitHub secrets): STRIPE_SECRET_KEY, STRIPE_WEBHOOK_SECRET,
// STRIPE_PRICE_ID, SIGNING_KEY (private JWK), ADMIN_TOKEN, optional RESEND_API_KEY + EMAIL_FROM.
import { DurableObject } from "cloudflare:workers";
import { Store } from "./store.js";
import { signResponse, verifyStripeSignature, timingSafeEqual } from "./crypto.js";
import { successPage, messagePage, adminPage } from "./pages.js";

export class Licenses extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.store = new Store(ctx.storage.sql);
    this.hits = new Map();
  }
  call(method, args) {
    if (typeof this.store[method] !== "function") throw new Error("unknown method");
    const result = this.store[method](...args);
    return result === undefined ? null : JSON.parse(JSON.stringify(result));
  }
  // Simple per-IP limiter (requests per minute) kept in memory.
  allow(ip, limit) {
    const minute = Math.floor(Date.now() / 60000);
    const entry = this.hits.get(ip);
    if (!entry || entry.minute !== minute) { this.hits.set(ip, { minute, n: 1 }); if (this.hits.size > 5000) this.hits.clear(); return true; }
    entry.n++;
    return entry.n <= limit;
  }
}

const json = (body, status = 200) => new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json", "cache-control": "no-store" } });
const html = (body, status = 200) => new Response(body, { status, headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" } });
const db = (env) => env.LICENSES.get(env.LICENSES.idFromName("main"));
const call = (env, method, ...args) => db(env).call(method, args);

async function stripe(env, method, path, form) {
  const res = await fetch("https://api.stripe.com/v1/" + path, {
    method,
    headers: { authorization: "Bearer " + env.STRIPE_SECRET_KEY, "content-type": "application/x-www-form-urlencoded" },
    body: form ? new URLSearchParams(form).toString() : undefined,
  });
  const data = await res.json();
  if (!res.ok) throw new Error(data?.error?.message || "Stripe request failed");
  return data;
}

async function issueForSession(env, session) {
  if (session.payment_status !== "paid") return null;
  const lic = await call(env, "issue", {
    session: session.id,
    payment: typeof session.payment_intent === "string" ? session.payment_intent : session.payment_intent?.id || null,
    email: (session.customer_details?.email || session.customer_email || "").toLowerCase(),
    maxDevices: Number(env.MAX_DEVICES || 2),
  });
  if (lic && lic.fresh) await emailKey(env, lic).catch(() => {});
  return lic;
}

async function emailKey(env, lic) {
  if (!env.RESEND_API_KEY || !env.EMAIL_FROM || !lic.email) return;
  await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: { authorization: "Bearer " + env.RESEND_API_KEY, "content-type": "application/json" },
    body: JSON.stringify({
      from: env.EMAIL_FROM,
      to: lic.email,
      subject: "Your LunarTweaks Pro key",
      html: `<p>Thanks for buying LunarTweaks Pro!</p><p>Your key:</p><p style="font-size:20px;font-family:monospace;letter-spacing:1px"><b>${lic.key}</b></p><p>Open LunarTweaks, paste the key and click <b>Activate Pro &amp; scan</b>. It works on ${lic.max_devices} PCs.</p>`,
    }),
  });
}

async function appApi(request, env, purpose) {
  let body;
  try { body = await request.json(); } catch { return json({ ok: false, code: "bad_request" }, 400); }
  const key = String(body.key || "").slice(0, 64), device = String(body.device || "").slice(0, 128);
  const nonce = String(body.nonce || "");
  if (!/^[a-f0-9]{32,64}$/.test(nonce)) return json({ ok: false, code: "bad_request" }, 400);
  const result = await call(env, purpose, { key, device, label: String(body.label || "") });
  const status = result.ok ? "granted" : result.code;
  const ts = Math.floor(Date.now() / 1000);
  const sig = await signResponse(env.SIGNING_KEY, purpose, nonce, device, status, ts);
  const messages = {
    not_found: "That key doesn't exist. Check it against your receipt.",
    revoked: "This key has been revoked.",
    device_limit: `This key is already active on ${result.limit || "its maximum number of"} PCs. Sign out on an old PC, or contact support.`,
    not_activated: "This PC isn't activated for this key yet.",
    bad_device: "This PC couldn't be identified.",
  };
  return json({ ok: !!result.ok, status, ts, sig, message: result.ok ? "" : messages[result.code] || "Key rejected." });
}

async function admin(request, env, url) {
  const auth = request.headers.get("authorization") || "";
  if (!env.ADMIN_TOKEN || !timingSafeEqual(auth, "Bearer " + env.ADMIN_TOKEN)) return json({ error: "unauthorized" }, 401);
  const action = url.pathname.slice("/admin/api/".length);
  const body = request.method === "POST" ? await request.json().catch(() => ({})) : {};
  switch (action) {
    case "stats": return json(await call(env, "stats"));
    case "recent": return json(await call(env, "recent", 50));
    case "find": return json(await call(env, "find", url.searchParams.get("q") || ""));
    case "revoke": return json({ ok: await call(env, "revoke", body.key, body.revoked !== false) });
    case "reset": return json({ ok: await call(env, "reset", body.key) });
    case "devices": return json({ ok: await call(env, "setDevices", body.key, Math.max(1, Math.min(50, Number(body.devices) || 1))) });
    case "issue": {
      const lic = await call(env, "issue", { email: String(body.email || "").toLowerCase(), note: String(body.note || "manual"), maxDevices: Number(body.devices || env.MAX_DEVICES || 2) });
      if (body.send) await emailKey(env, lic).catch(() => {});
      return json(lic);
    }
  }
  return json({ error: "unknown action" }, 404);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const ip = request.headers.get("cf-connecting-ip") || "unknown";
    const path = url.pathname.replace(/\/+$/, "") || "/";
    try {
      if (path.startsWith("/v1/") || path === "/buy") {
        if (!(await db(env).allow(ip, 40))) return json({ ok: false, code: "rate_limited", message: "Too many attempts. Wait a minute and try again." }, 429);
      }
      if (request.method === "POST" && path === "/v1/activate") return appApi(request, env, "activate");
      if (request.method === "POST" && path === "/v1/validate") return appApi(request, env, "validate");
      if (request.method === "POST" && path === "/v1/deactivate") {
        const body = await request.json().catch(() => ({}));
        return json(await call(env, "deactivate", { key: body.key, device: body.device }));
      }
      if (path === "/buy") {
        const session = await stripe(env, "POST", "checkout/sessions", {
          mode: "payment",
          "line_items[0][price]": env.STRIPE_PRICE_ID,
          "line_items[0][quantity]": "1",
          success_url: `${url.origin}/success?session_id={CHECKOUT_SESSION_ID}`,
          cancel_url: env.SITE_URL || url.origin,
          allow_promotion_codes: "true",
          "metadata[product]": "lunartweaks-pro",
        });
        return Response.redirect(session.url, 303);
      }
      if (path === "/success") {
        const id = url.searchParams.get("session_id") || "";
        if (!/^cs_[A-Za-z0-9_]+$/.test(id)) return html(messagePage("Payment not found", "This link is missing its payment reference.", env), 400);
        const session = await stripe(env, "GET", "checkout/sessions/" + encodeURIComponent(id));
        if (session.metadata?.product !== "lunartweaks-pro") return html(messagePage("Payment not found", "This payment isn't for LunarTweaks Pro.", env), 404);
        const lic = await issueForSession(env, session);
        if (!lic) return html(messagePage("Payment processing", "Your payment is still being confirmed. Refresh this page in a minute.", env), 202);
        return html(successPage(lic, env));
      }
      if (request.method === "POST" && path === "/stripe/webhook") {
        const raw = await request.text();
        if (!(await verifyStripeSignature(raw, request.headers.get("stripe-signature"), env.STRIPE_WEBHOOK_SECRET)))
          return json({ error: "bad signature" }, 400);
        const event = JSON.parse(raw);
        const obj = event.data?.object || {};
        if ((event.type === "checkout.session.completed" || event.type === "checkout.session.async_payment_succeeded") && obj.metadata?.product === "lunartweaks-pro")
          await issueForSession(env, obj);
        if (event.type === "charge.refunded" || event.type === "charge.dispute.created") {
          const pi = obj.payment_intent;
          if (pi) await call(env, "revokePayment", typeof pi === "string" ? pi : pi.id);
        }
        return json({ received: true });
      }
      if (path === "/admin") return html(adminPage(env));
      if (path.startsWith("/admin/api/")) return admin(request, env, url);
      if (path === "/") return Response.redirect(env.SITE_URL || "https://lunartweaks.se", 302);
      return json({ error: "not found" }, 404);
    } catch (error) {
      return path.startsWith("/v1/") || path.startsWith("/admin/api/")
        ? json({ ok: false, code: "server_error", message: "The key server had a problem. Try again shortly." }, 500)
        : html(messagePage("Something went wrong", "Please try again in a moment. If you were charged, your key is safe — reload this page.", env), 500);
    }
  },
};
