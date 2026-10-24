// Response signing (ECDSA P-256 / SHA-256, raw r||s) and Stripe webhook verification.
const enc = new TextEncoder();
const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");

let cachedKey = null, cachedJwk = null;
async function signingKey(jwkText) {
  if (cachedKey && cachedJwk === jwkText) return cachedKey;
  const jwk = JSON.parse(jwkText);
  cachedKey = await crypto.subtle.importKey("jwk", jwk, { name: "ECDSA", namedCurve: "P-256" }, false, ["sign"]);
  cachedJwk = jwkText;
  return cachedKey;
}

// The app rebuilds exactly this text and checks the signature with the public key compiled into it.
export function responseMessage(purpose, nonce, device, status, ts) {
  return `LunarTweaks/v2\n${purpose}\n${nonce}\n${device}\n${status}\n${ts}`;
}

export async function signResponse(jwkText, purpose, nonce, device, status, ts) {
  const key = await signingKey(jwkText);
  const sig = await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, key, enc.encode(responseMessage(purpose, nonce, device, status, ts)));
  return hex(sig);
}

export function timingSafeEqual(a, b) {
  const x = enc.encode(String(a)), y = enc.encode(String(b));
  let diff = x.length ^ y.length;
  for (let i = 0; i < Math.max(x.length, y.length); i++) diff |= (x[i] || 0) ^ (y[i] || 0);
  return diff === 0;
}

// Stripe-Signature: t=timestamp,v1=hex(HMAC_SHA256(secret, `${t}.${payload}`)). 5-minute tolerance.
export async function verifyStripeSignature(payload, header, secret, now = Date.now()) {
  if (!header || !secret) return false;
  const parts = Object.fromEntries(header.split(",").map((p) => p.split("=")).filter((p) => p.length === 2).map(([k, v]) => [k.trim(), v.trim()]));
  const signatures = header.split(",").map((p) => p.trim()).filter((p) => p.startsWith("v1=")).map((p) => p.slice(3));
  const t = Number(parts.t);
  if (!t || !signatures.length || Math.abs(now / 1000 - t) > 300) return false;
  const key = await crypto.subtle.importKey("raw", enc.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const expected = hex(await crypto.subtle.sign("HMAC", key, enc.encode(`${t}.${payload}`)));
  return signatures.some((s) => timingSafeEqual(s, expected));
}
