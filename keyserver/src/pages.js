// Buyer and owner pages, styled like lunartweaks.se (near-black violet, glow, rounded cards).
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

const shell = (title, body, script = "") => `<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>${esc(title)} · LunarTweaks</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link href="https://fonts.googleapis.com/css2?family=Manrope:wght@400;600;700&family=Sora:wght@600;700&display=swap" rel="stylesheet">
<style>
:root{--bg:#07060d;--card:#110f1b;--border:#262139;--text:#f4f2fa;--muted:#a6a0bb;--lav:#c0a0ff;--accent:#7c3aed;--accent2:#9f67ff;--green:#7ad6a0;--red:#ec7c88}
*{box-sizing:border-box}html,body{margin:0;min-height:100%}
body{background:radial-gradient(900px 520px at 50% -10%,rgba(124,58,237,.35),transparent 70%),radial-gradient(600px 400px at 85% 110%,rgba(80,90,255,.18),transparent 70%),var(--bg);
color:var(--text);font:16px/1.55 Manrope,system-ui,sans-serif;display:flex;align-items:center;justify-content:center;padding:32px 16px}
.card{width:100%;max-width:560px;background:var(--card);border:1px solid var(--border);border-radius:22px;padding:36px;box-shadow:0 30px 80px rgba(60,20,140,.25);animation:rise .6s cubic-bezier(.2,.8,.2,1) both}
.wide{max-width:980px}
@keyframes rise{from{opacity:0;transform:translateY(16px)}to{opacity:1;transform:none}}
h1{font:700 30px/1.2 Sora,sans-serif;margin:0 0 8px}h2{font:600 18px Sora,sans-serif;margin:24px 0 10px}
p{color:var(--muted);margin:0 0 16px}.brand{display:flex;align-items:center;gap:10px;font:700 18px Sora,sans-serif;margin-bottom:26px}
.brand b{color:var(--lav)}.dot{width:30px;height:30px;border-radius:50%;background:radial-gradient(circle at 35% 35%,#e3d4ff,#7c3aed 55%,#2a1458);box-shadow:0 0 24px rgba(124,58,237,.6)}
.key{display:flex;gap:10px;align-items:center;background:#0a0812;border:1px solid #3a2f5c;border-radius:14px;padding:16px 18px;margin:18px 0 8px}
.key code{flex:1;font:600 19px/1.3 ui-monospace,Consolas,monospace;letter-spacing:1px;color:#fff;word-break:break-all}
button,.btn{appearance:none;border:0;border-radius:999px;padding:12px 22px;font:700 15px Manrope,sans-serif;color:#fff;cursor:pointer;
background:linear-gradient(90deg,var(--accent),var(--accent2));box-shadow:0 8px 28px rgba(124,58,237,.35);text-decoration:none;display:inline-block;transition:transform .15s,filter .15s}
button:hover,.btn:hover{transform:translateY(-2px);filter:brightness(1.08)}button.ghost{background:#1e1a2f;box-shadow:none;border:1px solid var(--border)}
ol{color:var(--muted);padding-left:20px;margin:0}ol li{margin:6px 0}ol b{color:var(--text)}
.ok{color:var(--green)}.small{font-size:13px;color:#77708c}
input{width:100%;background:#0a0812;border:1px solid #3a2f5c;border-radius:12px;padding:12px 14px;color:#fff;font:15px Manrope,sans-serif}
table{width:100%;border-collapse:collapse;font-size:14px}td,th{text-align:left;padding:9px 8px;border-bottom:1px solid var(--border);vertical-align:top}
th{color:var(--muted);font-weight:600}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.pill{font-size:12px;padding:3px 10px;border-radius:99px;background:#1e1a2f}
.pill.bad{background:#3a1820;color:var(--red)}.pill.good{background:#14301f;color:var(--green)}
.stats{display:flex;gap:12px;margin:10px 0 0}.stat{flex:1;background:#0a0812;border:1px solid var(--border);border-radius:14px;padding:14px}.stat b{font:700 24px Sora;display:block}
</style></head><body>${body}${script ? `<script>${script}</script>` : ""}</body></html>`;

const brand = `<div class="brand"><div class="dot"></div><span>Lunar <b>Tweaks</b></span></div>`;

export function successPage(lic, env) {
  return shell("Your Pro key", `<main class="card">${brand}
<h1>You're Pro now 🎉</h1><p>Thanks for buying LunarTweaks Pro. Here is your key${lic.email ? ` — we also linked it to <b style="color:#fff">${esc(lic.email)}</b>` : ""}.</p>
<div class="key"><code id="k">${esc(lic.key)}</code><button id="c" class="ghost">Copy</button></div>
<p class="small">Save this key. It works on ${esc(lic.max_devices)} PCs, and you can move it by signing out in the app.</p>
<h2>Activate it</h2><ol><li>Open <b>LunarTweaks</b>.</li><li>Paste the key and click <b>Activate Pro &amp; scan</b>.</li><li>Click <b>Apply recommended</b>. Every change can be undone.</li></ol>
<p style="margin-top:24px"><a class="btn" href="${esc(env.SITE_URL || "https://lunartweaks.se")}">Back to LunarTweaks</a></p></main>`,
    `document.getElementById('c').onclick=async()=>{try{await navigator.clipboard.writeText(document.getElementById('k').textContent);document.getElementById('c').textContent='Copied ✓'}catch(e){}}`);
}

export function messagePage(title, text, env) {
  return shell(title, `<main class="card">${brand}<h1>${esc(title)}</h1><p>${esc(text)}</p>
<p><a class="btn" href="${esc((env && env.SITE_URL) || "https://lunartweaks.se")}">Back to LunarTweaks</a></p></main>`);
}

export function adminPage() {
  return shell("Admin", `<main class="card wide">${brand}<h1>Key admin</h1>
<div id="login"><p>Enter your admin token (the ADMIN_TOKEN secret).</p><div class="row"><input id="tok" type="password" style="flex:1"><button id="go">Sign in</button></div></div>
<div id="app" hidden>
<div class="stats"><div class="stat"><b id="s1">–</b><span class="small">keys</span></div><div class="stat"><b id="s2">–</b><span class="small">active</span></div><div class="stat"><b id="s3">–</b><span class="small">activated PCs</span></div></div>
<h2>Find a key</h2><div class="row"><input id="q" placeholder="Key, email or Stripe payment ID" style="flex:1"><button id="find">Search</button></div>
<h2>Give a key (manual sale, Swish by hand, giveaway)</h2><div class="row"><input id="em" placeholder="Buyer email (optional)" style="flex:1"><button id="issue">Create key</button></div>
<p id="issued" class="ok"></p>
<h2 id="title">Latest keys</h2><table><thead><tr><th>Key</th><th>Email</th><th>Created</th><th>PCs</th><th>Status</th><th></th></tr></thead><tbody id="rows"></tbody></table>
<p style="margin-top:18px"><button class="ghost" id="out">Sign out</button></p></div></main>`,
`const $=id=>document.getElementById(id);let token=sessionStorage.getItem('lt_admin')||'';
const api=async(p,body)=>{const r=await fetch('/admin/api/'+p,{method:body?'POST':'GET',headers:{authorization:'Bearer '+token,'content-type':'application/json'},body:body?JSON.stringify(body):undefined});if(r.status===401){sessionStorage.removeItem('lt_admin');location.reload();}return r.json()};
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function render(list,title){$('title').textContent=title;$('rows').innerHTML=list.map(l=>'<tr><td><code>'+esc(l.key)+'</code></td><td>'+esc(l.email||l.note||'')+'</td><td>'+new Date(l.created).toLocaleDateString()+'</td><td>'+(l.devices?l.devices.length+' / ':'')+l.max_devices+'</td><td>'+(l.revoked?'<span class="pill bad">revoked</span>':'<span class="pill good">active</span>')+'</td><td class="row"><button class="ghost" data-a="reset" data-k="'+esc(l.key)+'">Reset PCs</button><button class="ghost" data-a="'+(l.revoked?'unrevoke':'revoke')+'" data-k="'+esc(l.key)+'">'+(l.revoked?'Restore':'Revoke')+'</button></td></tr>').join('')||'<tr><td colspan=6 class="small">Nothing found.</td></tr>'}
async function load(){const s=await api('stats');$('s1').textContent=s.licenses;$('s2').textContent=s.active;$('s3').textContent=s.devices;render(await api('recent'),'Latest keys')}
$('rows').onclick=async e=>{const b=e.target.closest('button');if(!b)return;const k=b.dataset.k,a=b.dataset.a;if(a==='reset')await api('reset',{key:k});else await api('revoke',{key:k,revoked:a==='revoke'});load()};
$('find').onclick=async()=>render(await api('find?q='+encodeURIComponent($('q').value)),'Search results');
$('issue').onclick=async()=>{const l=await api('issue',{email:$('em').value,send:true});$('issued').textContent='New key: '+l.key;load()};
$('go').onclick=()=>{token=$('tok').value.trim();sessionStorage.setItem('lt_admin',token);start()};
$('out').onclick=()=>{sessionStorage.removeItem('lt_admin');location.reload()};
async function start(){if(!token)return;$('login').hidden=true;$('app').hidden=false;load()}start();`);
}
