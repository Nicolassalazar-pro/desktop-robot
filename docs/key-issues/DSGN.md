## Design [DSGN]
> Last reviewed: 2026-08-07 | Open: 14

### DSGN-1 — STOP is momentary and can be overridden by a still-touching stick within one control tick
- Severity: CRITICAL
- Status: OPEN
- File: `src/main.cpp:8488` (`stopAll`), `src/main.cpp:8414` (30 ms `setInterval`), `src/main.cpp:8438-8441` and `8477-8479` (stick `pointermove` handlers)
- Problem: `stopAll()` zeros `curL`/`curR` and fires a single control frame, but does not release the held pointer, does not visually re-centre the stick, and does not gate the periodic sender — the next `pointermove` on a still-held stick reassigns `curL`/`curR` back to the finger position and the 30 Hz interval resumes drive within one tick.
- Why: The only "emergency" surface on the app must survive the exact posture the user is in when they panic: one thumb on a stick, the other hand on STOP. Right now, any twitch of the held finger after STOP re-arms drive because the `pointermove` handler reads absolute stick position on every move (`const v=compute(e);setVal(v)`), and `setInterval(()=>sendCtrl(curL,curR),30)` immediately transmits the new non-zero value. This is exactly the "control that is ambiguous about whether the robot will move" case the design-craft skill §10 warns about, and it also fails §7 (perceived latency): the user watched the wheels stop for one frame and then start again for no visible reason.
- Fix: Make STOP a durable state, not a momentary write. Introduce a `stopLatched` flag set true by `stopAll()`, cleared only by a fresh `pointerdown` on either stick (or a Resume button). While latched: (a) `sendCtrl` and the 30 ms interval force `0,0`; (b) both stick handlers ignore `pointermove`; (c) `neutralUi()` re-centres every thumb; (d) the pill switches to `STOPPED`. That's the "quasimode → mode" fix from §8: a real emergency needs a mode, not a keypress.

### DSGN-2 — Emergency STOP is styled with the "everything is good" accent green, not a danger colour
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:8242` (`--danger:#ef4444` defined), `src/main.cpp:8254` (`.primary{background:var(--accent)…}`), `src/main.cpp:8351` (`<button class="primary stopBig" onclick="stopAll()">STOP</button>`)
- Problem: The full-width STOP button uses `.primary`, whose background is `--accent` (Spotify-green `#1db954`) — the exact same colour as the live-link dot, the active-mood highlight, `.face.picked` confirmation, the battery bar, the charging badge, and the stick thumbs. The one dangerous control on the page is chromatically identical to every positive state on the page.
- Why: Design-craft §10 is explicit: "High-consequence controls get unique shape, color, AND position." STOP has unique shape (full-width bar) and unique position (footer of every layout) — but no unique colour. §3 also states state-colours (`success/warn/error/info`) live outside the brand palette, and the file already defines `--danger:#ef4444`, which is used *nowhere*. A defined danger token with no danger consumer is §16's "a preference that a good default could erase" applied to a token.
- Fix: Add `.danger{background:var(--danger);color:var(--text)}` and change the button to `<button class="danger stopBig">`. Keep the same width and position (Fitts intent preserved). One line of CSS, one class swap — the emergency control now looks like an emergency control.

### DSGN-3 — Sticks give no visual cue when drive is LOCKED, WAIT-NEUTRAL, or the WebSocket is down
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:8265-8273` (`.stick-pad`, `.vstick` styles — no `[data-state]` variants), `src/main.cpp:8370-8372` (`paintLink`, `setDriveStatus` — only writes the header pill), `src/main.cpp:8404-8410` (`sendCtrl` silently returns when `ws.readyState!==1`)
- Problem: The only place drive-armed status is shown is the header pill (`#link`, ~11 px text, 4×10 px padding, top-right). The sticks look fully live in every state — LOCKED, WAIT NEUTRAL, ARMED, SAFE OFF, or WebSocket-dropped — and can be dragged with zero indication that the drive train is inert. `sendCtrl` returns silently on a closed socket; the pad returns silently on LOCKED. The user drags, feels nothing, doesn't know whether the robot, the phone, or the link is at fault.
- Why: Design-craft §8 "the mode you're in is always visible" and §11 (accessibility, non-colour state) both fail here. The interaction surface has to carry the state, not a corner pill — that's exactly the "content is the interface — direct manipulation first" principle from §1. In terms of operational impact, this happens on every session boot (the pad shows LOCKED for at least one telemetry frame after connect) and after every reconnect.
- Fix: Bind a `data-drive` attribute to `body` from `setDriveStatus`, and add:
  ```css
  body[data-drive="LOCKED"] .stick-pad, body[data-drive="LOCKED"] .vstick,
  body[data-drive="WAIT NEUTRAL"] .stick-pad, body[data-drive="WAIT NEUTRAL"] .vstick{
    opacity:.45;
  }
  body[data-drive="LOCKED"] .thumb, body[data-drive="LOCKED"] .vthumb{
    background:var(--surf2);box-shadow:none;
  }
  body:not([data-link="live"]) .stick-pad::after,
  body:not([data-link="live"]) .vstick::after{
    content:"reconnecting…";position:absolute;inset:0;display:grid;place-items:center;
    background:rgba(0,0,0,.6);color:var(--text);font:600 12px system-ui;letter-spacing:.05em;
  }
  ```
  Also add a body-class flip from `openWs`/`onclose` for the link state. The pill stays for detail; the pad now carries the mode.

### DSGN-4 — Main-view expression buttons are below the 44 px touch-target baseline
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:8263` (`.face{min-height:38px;padding:8px 4px}`), `src/main.cpp:8262` (`.faces{gap:6px}`), `src/main.cpp:8508` (`nameGrid($('expF'),['Faces','Sounds','Movement'],…)`)
- Problem: The three main expression buttons (`Faces`, `Sounds`, `Movement`) render at `min-height:38px` with `gap:6px`. The sheet-overridden `.sheet .face{min-height:48px}` correctly clears the baseline, but the main-view instances — the only entry point to every mood, sound, and gesture the user has — remain 38 px tall with 6 px separators.
- Why: Design-craft §11 baseline: "Targets ≥ 44×44 pt with ≥ 8 pt gap". §8 restates it. This is the user's most-tapped surface after the sticks; missing it because the button is 6 px shy of Fitts-safe is a papercut on every audition session.
- Fix: Change `.face` to `min-height:44px` and `.faces` to `gap:8px`. The sheet override then becomes decoration, not correction. No layout impact — the Expressions card grows by 6 px at most.

### DSGN-5 — Mood-active state is signalled by colour alone
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:8264` (`.face.active{border-color:var(--accent);background:rgba(29,185,84,.12);color:var(--accent)}`), `src/main.cpp:8374` (`paintFace` toggles only `.active`)
- Problem: The currently-active mood is distinguished from every other mood only by an accent-green tint (border, ~12 %-alpha background, text colour) — no shape, icon, weight, checkmark, or dot. Every other visual signal in this system uses the same green, so a colour-blind user, a bright-sun operator, or a screenshot on a monochrome printer cannot tell which mood is active.
- Why: Design-craft §3 non-negotiable: "Never convey state by color alone — pair with shape/icon/text." §11 restates the same rule as an accessibility driver. The mood grid is 16 items; the wrong-mood tap probability is 15/16 if you can't see the active one.
- Fix: Add a leading `▸` glyph or a bold weight (`.face.active{font-weight:700}`) or a small dot before the label: `.face.active::before{content:"● ";color:var(--accent)}`. Two lines, no layout jump.

### DSGN-6 — Wi-Fi QR pairing screen has no instruction line and prints SSID/password at 8-pixel type
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:9513-9543` (`drawQrScreenWifi`), specifically `src/main.cpp:9537-9538` (`qrCenterText(y=244, size=1, ssid)` and `qrCenterText(y=260, size=1, "pw:…")`)
- Problem: The Wi-Fi pairing screen shows a QR plus `Hanger OVERRIDE` and `pw:TheGarageAtl` in size-1 GFX text (6×8 px = ~1 mm tall). There is no title or instruction — the sibling `drawQrScreenUrl` at `9570` prints `"Scan to open control"`, but the Wi-Fi screen has no equivalent. The user is expected to guess what the QR is for, then squint at 1 mm text if their scanner didn't auto-fill.
- Why: The pairing screen is hit on *every* boot before the phone is connected — highest possible frequency in the flow. Design-craft §14 (embedded): "bias to physical-button rhythm"/larger. §4: text roles, body ≥ 14 pt / 16 pt. §5 §1 signifiers: "on flat/e-ink UIs a hidden interaction *does not exist*." Right now the interaction is hidden until the user has learned the flow once.
- Fix: (a) Add a title at y=8 or below the QR: `qrCenterText(y=232, size=2, "Scan to join Wi-Fi")`. (b) Bump SSID and password to `size=2` (12×16), regenerate y coordinates — the QR ends at y=228, so the labels move to `y=234` and `y=254` (both are still 6×2×len wide, `Hanger OVERRIDE` = 14 chars = 168 px, fits). If space is tight, drop `pw:` and use two lines: `SSID:` above `PW:`.

### DSGN-7 — Dead `#hint` element in the CSS (5 rules, ~380 bytes, no element, no toggler)
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:8279-8283` (`#hint`, `#hint.show`, `#hint b`, `#hint button`, `@keyframes up` — the last is shared with `.sheet .p` so keep it)
- Problem: `#hint` is styled, animated, and given a `.show` state, but there is no `id=hint` element in the DOM and no `.classList.add('show')`/`document.getElementById('hint')` reference anywhere in the script.
- Why: Design-craft §1 (Rams #10, "less but better"): every rule pays a maintenance cost. In a hand-rolled inlined UI it also signals to future editors that a hint pattern exists, inviting reintroduction of a dead pathway.
- Fix: Delete lines 8279-8283 (keep `@keyframes up` on 8281 — the sheet uses it). Roughly:
  ```css
  /* remove #hint block */
  ```

### DSGN-8 — `--danger` token defined but never consumed
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:8242` (`--danger:#ef4444` in `:root`)
- Problem: `--danger` is declared but no selector reads it. Meanwhile the actual danger control (STOP) uses `--accent`.
- Why: Design-craft §3 "state colors live OUTSIDE the brand palette" and §16 anti-patterns. A defined semantic token with no consumer is a broken promise — it implies "we handle destructive state" while the UI does not.
- Fix: Consume it in the `.danger` class introduced by DSGN-2. If DSGN-2 is rejected, delete the token: `:root{…--accent:#1db954}` (drop the trailing `;--danger:#ef4444`).

### DSGN-9 — Battery meter bar stays accent-green at every charge level
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:8259-8260` (`.bar>div{background:var(--accent)}`), `src/main.cpp:8395-8398` (`$('pctbar').style.width=pct+'%'`)
- Problem: The battery bar fills accent-green regardless of `pct`. A 3 %-remaining bar and a 100 %-full bar both read as "healthy green" until the user reads the tabular-nums text next to it. When the whole point of the bar is a glance-check, this defeats the bar.
- Why: This isn't state-by-colour-alone (there's a % number), but the bar is graphically decorative rather than informative. Design-craft §5 content-density: "each element justifies its cost". A three-level colour ramp turns the bar into real signal.
- Fix: Two-line JS in `onmessage`:
  ```js
  const bar=$('pctbar');bar.style.width=pct+'%';
  bar.style.background=pct<15?'var(--danger)':pct<30?'#d29922':'var(--accent)';
  ```
  Consumes `--danger` (see DSGN-8) and adds one warn tone (borrowed from the amber your history's melody notes reference). No new tokens required if you accept the amber inline.

### DSGN-10 — Token block bypassed by roughly a dozen hard-coded hex/rgba values
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:8248` (`rgba(29,185,84,.18)`), `8250` (`rgba(29,185,84,.16)`), `8254` (`color:#0a0a0a`), `8264` (`rgba(29,185,84,.12)`), `8265` (`#1a1a1a`,`#0d0d0d`), `8266` (`rgba(255,255,255,.06)`), `8267` (`#3ad975`,`#1db954`,`#0e5a2a`, `rgba(29,185,84,.45)`, `rgba(255,255,255,.1)`), `8269` (`#161616`,`#0d0d0d`), `8270-8271` (`rgba(255,255,255,.05)`, `rgba(255,255,255,.06)`), `8272` (repeats `.thumb` colours), `8284` (`rgba(0,0,0,.7)`), `8293` (`color:#0a0a0a`)
- Problem: The `:root` block advertises a semantic system (`--bg --surf --surf2 --text --muted --accent --danger`), but the actual paint uses hard-coded hex or rgba variants of those same tones in more than a dozen sites, so a future palette swap (dark mode-alt, high-contrast, tinted accent) would need edits across the CSS instead of one root override.
- Why: Design-craft §3: "Semantic, never literal … This is what makes dark-mode / high-contrast / brand-swap all ride one layer." The token layer is currently theatre — it names things but does not carry the paint.
- Fix: Add the missing semantic tokens and route through them:
  ```css
  :root{--bg:#0a0a0a;--surf:#141414;--surf2:#1c1c1c;--surf-lo:#0d0d0d;--surf-hi:#1a1a1a;
        --text:#f3f3f3;--muted:#9aa0a6;--accent:#1db954;--accent-lo:#0e5a2a;--accent-hi:#3ad975;
        --danger:#ef4444;--overlay-6:rgba(255,255,255,.06);--accent-a12:rgba(29,185,84,.12);
        --accent-a16:rgba(29,185,84,.16);--accent-a18:rgba(29,185,84,.18);--accent-a45:rgba(29,185,84,.45);
        --scrim:rgba(0,0,0,.7)}
  ```
  Then rewrite each affected rule to reference the tokens (e.g. `.thumb{background:radial-gradient(circle at 35% 35%,var(--accent-hi) 0%,var(--accent) 50%,var(--accent-lo) 100%)}`).

### DSGN-11 — Faces/Sounds/Movement buttons flash the "audition-confirmed" green when they only open a sheet
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:8497-8498` (`flashPick` + `nameGrid`), `src/main.cpp:8508` (main-view Expressions grid also uses `nameGrid`)
- Problem: `.face.picked` was introduced to give tap feedback for actions with no persistent state (sounds, movements — comment at 8290-8292). But `nameGrid` is also reused to build the main-view Expressions row, so tapping `Faces` flashes accent-green as if a face were played, then opens the mood sheet. The user gets a false "action fired" cue for a control that only opens a picker.
- Why: Design-craft §1 signifiers vs affordances and §5 named actions: feedback semantics should map to what actually happened. Accent-green flash currently means "the robot got a command" everywhere else on the surface.
- Fix: Split the helper. Add a `sheetGrid` that omits `flashPick`:
  ```js
  function sheetGrid(host,names,fn){host.innerHTML='';names.forEach((n,i)=>{
    const b=document.createElement('button');b.className='face';b.textContent=n;
    b.onclick=()=>fn(i);host.appendChild(b);});}
  ```
  Use `sheetGrid` for the top-row `expF`, keep `nameGrid` for the audition sheets (`allS`, `allM`). Alternatively, style the top row with a distinct "opens picker" affordance (a `▾` on the label) and drop the flash there.

### DSGN-12 — Main-view expression buttons labelled with lowercase category nouns instead of named actions
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:8263` (`.face{…text-transform:lowercase}`), `src/main.cpp:8508` (labels `['Faces','Sounds','Movement']`)
- Problem: The `text-transform:lowercase` on `.face` — applied because mood tags read best as lowercase words like `curious` — also lowercases the three top-row category buttons, so the user sees `faces`, `sounds`, `movement`. These are nouns, not actions, and they don't say what happens on tap.
- Why: Design-craft §5: "Named, imperative action buttons — never 'OK'." §16 anti-pattern: "'OK'/'Cancel' instead of named verbs." The three buttons are the only entry points into ~22 expressions; they should tell the user what tapping does. `faces` doesn't; `Pick a face` or `Face…` does.
- Fix: Scope the transform to the sheet variants only, and rename:
  ```css
  .sheet .face{text-transform:lowercase}  /* mood/gesture/sound tags */
  .face{ /* remove lowercase here */ }
  ```
  ```js
  nameGrid($('expF'),['Face…','Sound…','Move…'],…);
  ```
  The `…` conveys "opens picker" (§5), the capital tells the user this is a named surface.

### DSGN-13 — LCD "Establishing" splash is under-specified (one word, no progress signal)
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:9711-9715` (`drawEstablishingScreen`) — draws only `qrCenterTextDark(y=120, size=2, "Establishing")`; the comment above at 9709-9710 promises `"Establishing Server..."` but the actual literal is just `"Establishing"`.
- Problem: The splash the user sees while the AP boots is one word, size 2, no ellipsis, no dot animation, no "Wi-Fi" or "Server" qualifier. It contradicts its own inline documentation and gives the operator no read on whether the board is progressing or hung.
- Why: Design-craft §7 (latency): "> 1 s needs a determinate progress signal." AP boot on this board is >1 s in practice. §14 embedded: bias legibility up; §14 e-ink line about "reserve one deliberate full-refresh flash for true repaints" — the boot is that repaint, name it.
- Fix: Match the comment. Either `"Establishing Wi-Fi"` (accurate) or `"Establishing Server…"` (matches comment), plus a growing-dot or scanning-bar drawn at ~1 Hz below the text. Zero flash cost — one draw call in the existing 200 ms loop.

### DSGN-14 — Off-scale spacing values scattered through the layout (6, 10, 18, 22, 34)
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:8262` (`.faces gap:6px`), `8297` (`.grid gap:10px;padding:12px`), `8299` (`.joy-card gap:10px`), `8301` (`.joy-vals gap:18px`), `8308` (`.grid gap:6px;padding:6px`), `8311-8312` (`.center gap:6px`, `.center .card padding:10px`), `8277` (`.vval bottom:22px`), `8267` (`.thumb width/height:64px` — on scale — but see `.thumb` inset offsets `35% 35%`)
- Problem: The 4/8-pt spacing rhythm (`4 8 12 16 24 32 48 64`) advertised by design-craft §4 is broken by 6, 10, 18, 22, and 34 px gaps and paddings. Each individually is invisible; the accumulation is what removes the "engineered" feel from the layout.
- Why: Design-craft §4: "8-pt spacing grid (4-pt half-step for icon padding only)". §12 (quiet luxury): "consistency of response, not richness of motion". The grid is your response.
- Fix: Normalise: `6→8`, `10→8` or `12`, `18→16`, `22→24`, `34→32`. Concretely: `.faces gap:8`, `.grid` portrait `gap:12 padding:12`, `.grid` landscape `gap:8 padding:8` (or `6` deliberately, as the landscape budget is tight — if so, note it inline), `.joy-vals gap:16`, `.center gap:8`, `.center .card padding:12`, `.vval bottom:24`. One pass, no layout regression.

---

## End-of-review summary

- **New DSGN findings by severity:** 1 CRITICAL · 5 HIGH · 5 MEDIUM · 3 LOW
- **Highest-leverage single change:** DSGN-1 (durable STOP). The whole safety story on the app rests on it, and it survives every layout the app will ever have.
- **Where the two surfaces contradict each other:**
  1. The LCD gesture-menu pressed-state uses a filled block + colour change ("reads instantly and without relying on colour perception alone", comment at 9615-9617), acknowledging §11 explicitly. The web app's `.face.active` state (DSGN-5) uses colour alone. Same design rule, opposite conclusions on the two surfaces.
  2. LCD `drawGestureMenu` treats `TEST` as a high-consequence surface (accent border, uppercase, size 2 label, exit-hint footer). The web STOP — a strictly higher-consequence control — has less visual emphasis than that TEST button relative to its neighbours (DSGN-2).
  3. The LCD's Establishing splash and QR screens use `size 2` for anything worth reading; the web app drops to `9 px .vlabel` and `11 px .badge`/`.subhead`. The embedded rhythm on the LCD is "bias up"; the web app biases down. Pick one.

Kept despite looking suspicious:
- Expression sheets stay open after tap (audition intent) — explicitly locked, do not touch.
- STOP full-width in both orientations — Fitts, explicitly locked.
- Page weight (~18 KB) — measured non-binding, explicitly out of scope.
- `.face.picked` 260 ms flash — necessary tap feedback for stateless channels; the finding is on where it's *re-used* (DSGN-11), not on the flash itself.
