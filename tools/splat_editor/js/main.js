// Splat editor: hand-made Gaussian splat models. Selection, transforms (G/R/S like Blender), construction tools,
// inspector, undo, files (.splats text, INRIA .ply), autosave.

const STORE_MODEL = 'splat_editor.model', STORE_TITLE = 'splat_editor.title';
const FOV = 45 * Math.PI / 180;

const E = {
    list: [], title: 'meadow_plant', cursor: [0, 0, 0], mode: 0, lit: true, grid: true, centres: false,
    tool: 'chain', tool_P: {}, undo: [], redo: [], xf: null, editing: false, mouse: [0, 0],
    cam: { az: 0.7, el: 0.35, dist: 1.2, target: [0, 0.12, 0] }, R: null, view_key: '', data_dirty: true, msg: '', msg_t: 0,
};

function $(id) { return document.getElementById(id); }
function sel_list() { return E.list.filter(t => t.sel); }
function msg(s) { E.msg = s; E.msg_t = performance.now(); }

// ---------------------------------------------------------------------------------------------------------------
// Undo, change notification, autosave

function snapshot() { return JSON.stringify(E.list); }
function restore(s) { E.list = JSON.parse(s); }
function push_undo() { E.undo.push(snapshot()); if (E.undo.length > 200) E.undo.shift(); E.redo.length = 0; }
function do_undo() { if (!E.undo.length) return msg('nothing to undo'); E.redo.push(snapshot()); restore(E.undo.pop()); changed(true); }
function do_redo() { if (!E.redo.length) return msg('nothing to redo'); E.undo.push(snapshot()); restore(E.redo.pop()); changed(true); }

// Starts an edit (undo step) once per drag; 'final' ends it.
function edit(final) { if (!E.editing) { push_undo(); E.editing = true; } if (final) E.editing = false; }

let save_timer = 0;
function changed(rebuild_panel)
{
    E.data_dirty = true;
    if (rebuild_panel) inspector_build();
    clearTimeout(save_timer);
    save_timer = setTimeout(() => { try { localStorage.setItem(STORE_MODEL, splats_write(E.list, E.title)); localStorage.setItem(STORE_TITLE, E.title); } catch (e) { /* ignore */ } }, 300);
}

// ---------------------------------------------------------------------------------------------------------------
// Camera, rays, picking

function camera()
{
    const c = E.cam, ce = Math.cos(c.el), cv = E.R.canvas;
    const eye = [c.target[0] + Math.sin(c.az) * ce * c.dist, c.target[1] + Math.sin(c.el) * c.dist, c.target[2] + Math.cos(c.az) * ce * c.dist];
    const aspect = cv.clientWidth / Math.max(cv.clientHeight, 1);
    return { eye: eye, view: m4_lookat(eye, c.target, [0, 1, 0]), proj: m4_perspective(FOV, aspect, c.dist * 0.005, c.dist * 200),
             focal: 0.5 / Math.tan(FOV / 2), dist: c.dist, aspect: aspect };
}

function ray_at(mx, my)
{
    const cam = camera(), cv = E.R.canvas, r = cv.getBoundingClientRect();
    const x = ((mx - r.left) / r.width) * 2 - 1, y = 1 - ((my - r.top) / r.height) * 2, t = Math.tan(FOV / 2);
    const fwd = vnorm(vsub(E.cam.target, cam.eye)), right = vnorm(vcross(fwd, [0, 1, 0])), up = vcross(right, fwd);
    return { o: cam.eye, d: vnorm(vadd(fwd, vadd(vscale(right, x * t * cam.aspect), vscale(up, y * t)))), h: r.height };
}

// Nearest splat whose 2-sigma ellipsoid (at least ~4 px wide) the ray hits.
function pick(mx, my)
{
    const ray = ray_at(mx, my), t = Math.tan(FOV / 2);
    let best = -1, bt = Infinity;
    E.list.forEach(function (s, i) {
        const minr = vlen(vsub(s.p, ray.o)) * 2 * t / ray.h * 4;
        const qi = [s.q[0], -s.q[1], -s.q[2], -s.q[3]];
        const k = s.s.map(v => Math.max(2 * v, minr));
        const o = q_rotate(qi, vsub(ray.o, s.p)).map((v, j) => v / k[j]), d = q_rotate(qi, ray.d).map((v, j) => v / k[j]);
        const a = vdot(d, d), b = vdot(o, d), c = vdot(o, o) - 1, disc = b * b - a * c;
        if (disc < 0) return;
        const th = (-b - Math.sqrt(disc)) / a;
        if (th > 0 && th < bt) { bt = th; best = i; }
    });
    return { i: best, p: best >= 0 ? vmad(ray.o, ray.d, bt) : null };
}

function to_screen(p)
{
    const cam = camera(), vp = m4_mul(cam.proj, cam.view), r = E.R.canvas.getBoundingClientRect();
    const x = vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12], y = vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13];
    const w = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
    return [r.left + (x / w * 0.5 + 0.5) * r.width, r.top + (0.5 - y / w * 0.5) * r.height];
}

// ---------------------------------------------------------------------------------------------------------------
// Transforms: G (grab), R (rotate), S (scale); X/Y/Z constrain (grab/rotate: world axis, scale: local axis);
// Shift = fine; LMB / Enter confirm, RMB / Esc cancel.

const AXES = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];

function xf_start(mode, no_undo)
{
    const s = sel_list();
    if (!s.length) return msg('nothing selected');
    if (!no_undo) push_undo();
    let pv = [0, 0, 0];
    for (const t of s) pv = vadd(pv, t.p);
    pv = vscale(pv, 1 / s.length);
    E.xf = { mode: mode, axis: -1, items: s.map(t => ({ t: t, p: t.p.slice(), s: t.s.slice(), q: t.q.slice() })), pivot: pv, m0: E.mouse.slice(), keep_undo: !!no_undo };
    xf_update();
}

function xf_update()
{
    const X = E.xf;
    if (!X) return;
    let m = E.mouse;
    if (X.fine) m = vadd(X.fine_m0, vscale(vsub([E.mouse[0], E.mouse[1], 0], X.fine_m0), 0.1)).slice(0, 2);
    const ps = to_screen(X.pivot), dx = m[0] - X.m0[0], dy = m[1] - X.m0[1];
    const cam = camera(), h = E.R.canvas.getBoundingClientRect().height;
    for (const it of X.items) { it.t.p = it.p.slice(); it.t.s = it.s.slice(); it.t.q = it.q.slice(); }
    if (X.mode === 'grab') {
        let d;
        if (X.axis >= 0) {
            const a = AXES[X.axis], s1 = to_screen(vadd(X.pivot, a)), sd = [s1[0] - ps[0], s1[1] - ps[1]];
            d = vscale(a, (dx * sd[0] + dy * sd[1]) / Math.max(sd[0] * sd[0] + sd[1] * sd[1], 1e-9));
        } else {
            const pw = vlen(vsub(X.pivot, cam.eye)) * 2 * Math.tan(FOV / 2) / h;
            const fwd = vnorm(vsub(E.cam.target, cam.eye)), right = vnorm(vcross(fwd, [0, 1, 0])), up = vcross(right, fwd);
            d = vadd(vscale(right, dx * pw), vscale(up, -dy * pw));
        }
        for (const it of X.items) it.t.p = vadd(it.p, d);
    } else if (X.mode === 'rotate') {
        const a0 = Math.atan2(X.m0[1] - ps[1], X.m0[0] - ps[0]), a1 = Math.atan2(m[1] - ps[1], m[0] - ps[0]);
        const fwd = vnorm(vsub(X.pivot, cam.eye));
        const ax = X.axis >= 0 ? AXES[X.axis] : fwd;
        const sgn = vdot(ax, fwd) > 0 ? -1 : 1;       // screen y points down
        const q = q_axis(ax, sgn * -(a1 - a0));
        for (const it of X.items) { it.t.p = vadd(X.pivot, q_rotate(q, vsub(it.p, X.pivot))); it.t.q = q_norm(q_mul(q, it.q)); }
        X.angle = (a1 - a0) * 180 / Math.PI;
    } else {
        const r0 = Math.hypot(X.m0[0] - ps[0], X.m0[1] - ps[1]), r1 = Math.hypot(m[0] - ps[0], m[1] - ps[1]);
        const f = Math.max(r1 / Math.max(r0, 1), 1e-3);
        for (const it of X.items) {
            if (X.axis >= 0) it.t.s[X.axis] = Math.max(it.s[X.axis] * f, 1e-5);
            else { it.t.s = it.s.map(v => Math.max(v * f, 1e-5)); it.t.p = vadd(X.pivot, vscale(vsub(it.p, X.pivot), f)); }
        }
        X.factor = f;
    }
    E.data_dirty = true;
}

function xf_end(ok)
{
    const X = E.xf;
    if (!X) return;
    if (!ok) { for (const it of X.items) { it.t.p = it.p; it.t.s = it.s; it.t.q = it.q; } if (!X.keep_undo) E.undo.pop(); }
    E.xf = null;
    changed(true);
}

// ---------------------------------------------------------------------------------------------------------------
// Edit operations

function select_only(list) { for (const t of E.list) t.sel = false; for (const t of list) t.sel = true; }

function add_splat()
{
    push_undo();
    const t = splat_make(E.cursor, [0.02, 0.02, 0.004], q_ident(), [0.42, 0.56, 0.23], 0.9);
    E.list.push(t); select_only([t]); changed(true);
    msg('new splat at the cursor');
}

function duplicate()
{
    const s = sel_list();
    if (!s.length) return msg('nothing selected');
    push_undo();                  // one undo step for duplicate + move; cancelling the move keeps the copies in place
    const c = s.map(splat_clone);
    for (const t of c) E.list.push(t);
    select_only(c); changed(true);
    xf_start('grab', true);
}

function delete_sel()
{
    const n = sel_list().length;
    if (!n) return msg('nothing selected');
    push_undo();
    E.list = E.list.filter(t => !t.sel);
    changed(true); msg('deleted ' + n);
}

function apply_tool()
{
    const T = TOOLS[E.tool], P = E.tool_P[E.tool], s = sel_list();
    if (T.uses === 'selection' && !s.length) return msg(T.name + ': select splats first');
    push_undo();
    let out;
    if (E.tool === 'radial') { out = tool_radial(s, E.cursor, P); E.list = E.list.filter(t => !t.sel); }
    else if (E.tool === 'mirror') out = tool_mirror(s, E.cursor);
    else if (E.tool === 'jitter') { out = tool_jitter_color(s, P); E.list = E.list.filter(t => !t.sel); }
    else out = T.fn(E.cursor, P);
    for (const t of out) E.list.push(t);
    select_only(out); changed(true);
    msg(T.name + ': ' + out.length + ' splats');
}

function frame_view()
{
    const s = sel_list().length ? sel_list() : E.list;
    if (!s.length) { E.cam.target = E.cursor.slice(); return; }
    let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
    for (const t of s) {
        const r = 2 * Math.max(t.s[0], t.s[1], t.s[2]);
        for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], t.p[k] - r); hi[k] = Math.max(hi[k], t.p[k] + r); }
    }
    E.cam.target = vscale(vadd(lo, hi), 0.5);
    E.cam.dist = Math.max(vlen(vsub(hi, lo)) * 1.4, 0.02);
}

// ---------------------------------------------------------------------------------------------------------------
// Panel: selection inspector and tools

function inspector_build()
{
    const box = $('selection');
    box.innerHTML = '';
    const s = sel_list();
    box.appendChild(el('h3', null, 'Selection <span class="hint">' + s.length + ' of ' + E.list.length + '</span>'));
    if (!s.length) { box.appendChild(el('div', 'hint', 'Click a splat to select it (Shift: add). N adds a splat at the cursor; the tools below build parts.')); return; }
    const set_all = (f, final) => { edit(final); for (const t of s) f(t); changed(false); };
    if (s.length === 1) {
        const t = s[0];
        const grp = (title, rows) => { const g = el('div', 'pgroup'); g.appendChild(el('div', 'gtitle', title)); for (const r of rows) g.appendChild(r); box.appendChild(g); };
        grp('position (m)', [0, 1, 2].map(k => ui_bar('xyz'[k], { min: -5, max: 5, step: 0.0005, get: () => t.p[k], set: (v, f) => { edit(f); t.p[k] = v; changed(false); } })));
        grp('sigma (m) — visible edge at ~2σ', [0, 1, 2].map(k => ui_bar('s' + 'xyz'[k], { min: 1e-4, max: 1, log: true, get: () => t.s[k], set: (v, f) => { edit(f); t.s[k] = v; changed(false); } })));
        grp('rotation (deg, X then Y then Z)', [0, 1, 2].map(k => ui_bar('r' + 'xyz'[k], { min: -180, max: 180, step: 0.5, get: () => q_to_euler(t.q)[k],
            set: (v, f) => { edit(f); const e = q_to_euler(t.q); e[k] = v; t.q = q_from_euler(e); changed(false); } })));
        grp('appearance', [ui_color('colour', () => rgb_hex(t.c), (v, f) => { edit(f); t.c = hex_rgb(v); changed(false); }),
                           ui_bar('opacity', { min: 0, max: 1, step: 0.01, get: () => t.a, set: (v, f) => { edit(f); t.a = v; changed(false); } })]);
    } else {
        const g = el('div', 'pgroup');
        g.appendChild(el('div', 'gtitle', 'all selected'));
        g.appendChild(ui_color('colour', () => rgb_hex(s[0].c), (v, f) => set_all(t => { t.c = hex_rgb(v); }, f)));
        g.appendChild(ui_bar('opacity', { min: 0, max: 1, step: 0.01, get: () => s[0].a, set: (v, f) => set_all(t => { t.a = v; }, f) }));
        box.appendChild(g);
    }
}

function tools_build()
{
    const box = $('tools');
    box.innerHTML = '';
    box.appendChild(el('h3', null, 'Tools'));
    const sel = el('select');
    for (const id of Object.keys(TOOLS)) { const o = el('option', null, TOOLS[id].name); o.value = id; sel.appendChild(o); }
    sel.value = E.tool;
    sel.addEventListener('change', () => { E.tool = sel.value; tools_build(); });
    box.appendChild(sel);
    const T = TOOLS[E.tool], P = E.tool_P[E.tool];
    box.appendChild(el('div', 'hint', T.uses === 'cursor' ? 'Builds new splats at the 3D cursor (C: cursor to mouse).' : 'Works on the selection; pivot = the 3D cursor.'));
    for (const d of T.params) {
        if (typeof d[1] === 'string') box.appendChild(ui_color(d[0], () => P[d[0]], v => { P[d[0]] = v; }));
        else box.appendChild(ui_bar(d[0], { min: d[1], max: d[2], step: d[4], log: d[1] > 0 && d[2] / d[1] >= 100, get: () => P[d[0]], set: v => { P[d[0]] = v; } }));
    }
    box.appendChild(ui_button('Apply (T)', apply_tool));
}

// ---------------------------------------------------------------------------------------------------------------
// Key bar and help

const KEYBAR = {
    edit: ['EDIT', ['LMB', 'select (Shift: add)'], ['G R S', 'move / rotate / scale'], ['N', 'new splat'], ['Shift+D', 'duplicate'], ['Del', 'delete'],
           ['A', 'select all'], ['Alt+A', 'none'], ['C', 'cursor to mouse'], ['T', 'apply tool'], ['1 2 3', 'splats / ellipsoids / centres'],
           ['L', 'lighting'], ['F', 'frame'], ['Ctrl+Z / Y', 'undo / redo'], ['RMB', 'orbit'], ['MMB', 'pan'], ['Wheel', 'zoom'], ['H', 'help']],
    xf: ['TRANSFORM', ['X Y Z', 'axis (scale: local axis)'], ['Shift', 'fine'], ['LMB / Enter', 'confirm'], ['RMB / Esc', 'cancel']],
    help: ['HELP', ['PgUp/PgDn', 'scroll'], ['Esc', 'close']],
};

function keybar_update()
{
    const mode = !$('help').classList.contains('hidden') ? 'help' : E.xf ? 'xf' : 'edit';
    const key = mode + (E.xf ? E.xf.mode + E.xf.axis : '');
    if (E.keybar_key === key) return;
    E.keybar_key = key;
    const k = KEYBAR[mode];
    let h = '<span class="slot mode">' + (E.xf ? E.xf.mode.toUpperCase() + (E.xf.axis >= 0 ? ' ' + 'XYZ'[E.xf.axis] : '') : k[0]) + '</span>';
    for (const e of k.slice(1)) h += '<span class="slot"><kbd>' + e[0] + '</kbd>' + e[1] + '</span>';
    $('keybar').innerHTML = h;
}

function help_text()
{
    return 'SPLAT EDITOR - hand-made Gaussian splat models (no capture data)\n\n' +
        'A splat is a 3D Gaussian: position, three standard deviations (sigma) along its own axes, a rotation, colour and opacity.\n' +
        'It is drawn as a soft ellipse facing the camera; the visible edge is at about 2 sigma. A flat splat (one sigma small)\n' +
        'is a leaf patch, a long thin one a blade or needle segment, a round one a blob.\n\n' +
        'SELECT   LMB on a splat (Shift: toggle); LMB on empty space: none; A all, Alt+A none\n' +
        'MOVE     G, then mouse; X/Y/Z lock to a world axis; Shift fine; LMB/Enter confirm, RMB/Esc cancel\n' +
        'ROTATE   R, then mouse around the selection centre; X/Y/Z: around a world axis, else the view axis\n' +
        'SCALE    S, then mouse away from / towards the centre; X/Y/Z: only that local sigma, else all (and positions)\n' +
        'CREATE   N: one splat at the 3D cursor; Shift+D: duplicate and move; Del: delete\n' +
        'CURSOR   C: put the 3D cursor where the mouse points (on a splat, else on the ground plane)\n' +
        'TOOLS    panel on the right: Chain (blade, stem), Leaf, Blob build at the cursor; Radial copies, Mirror and Colour\n' +
        '         variation work on the selection with the cursor as pivot. T applies the current tool.\n' +
        'VIEW     RMB drag orbit, MMB (or Shift+RMB) drag pan, wheel zoom, F frame selection (or all)\n' +
        '         1 splats, 2 ellipsoids (2 sigma), 3 centres; L lighting (normal = shortest axis); G grid via toolbar\n' +
        'UNDO     Ctrl+Z, Ctrl+Y (or Ctrl+Shift+Z)\n' +
        'FILES    .splats: one splat per line "x y z  sx sy sz  qw qx qy qz  #rrggbb  opacity" (metres, sigma)\n' +
        '         .ply: INRIA 3D Gaussian Splatting format (SH degree 0), opens in SuperSplat / SIBR; import reads it back\n';
}

// ---------------------------------------------------------------------------------------------------------------
// Input

function view_input()
{
    const c = E.R.canvas;
    let drag = null;
    c.addEventListener('contextmenu', e => e.preventDefault());
    c.addEventListener('mousedown', e => {
        c.focus();
        E.mouse = [e.clientX, e.clientY];
        if (E.xf) { xf_end(e.button === 0); e.preventDefault(); return; }
        if (e.button === 0) {
            const h = pick(e.clientX, e.clientY);
            if (h.i >= 0) { const t = E.list[h.i]; if (e.shiftKey) t.sel = !t.sel; else select_only([t]); }
            else if (!e.shiftKey) select_only([]);
            changed(true);
            return;
        }
        drag = { x: e.clientX, y: e.clientY, pan: e.button === 1 || e.shiftKey };
        e.preventDefault();
    });
    window.addEventListener('mouseup', () => { drag = null; });
    window.addEventListener('mousemove', e => {
        E.mouse = [e.clientX, e.clientY];
        if (E.xf) { xf_update(); return; }
        if (!drag) return;
        const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
        drag.x = e.clientX; drag.y = e.clientY;
        if (drag.pan) {
            const s = E.cam.dist * 0.0016, cam = camera();
            const fwd = vnorm(vsub(E.cam.target, cam.eye)), right = vnorm(vcross(fwd, [0, 1, 0])), up = vcross(right, fwd);
            E.cam.target = vadd(E.cam.target, vadd(vscale(right, -dx * s), vscale(up, dy * s)));
        } else { E.cam.az -= dx * 0.008; E.cam.el = clamp(E.cam.el + dy * 0.008, -1.5, 1.5); }
    });
    c.addEventListener('wheel', e => { e.preventDefault(); E.cam.dist = clamp(E.cam.dist * Math.pow(1.0015, e.deltaY), 0.01, 500); }, { passive: false });
}

function on_key(e)
{
    const k = e.key, ae = document.activeElement, tag = ae ? ae.tagName : '';
    const help = $('help');
    if (!help.classList.contains('hidden')) {
        if (k === 'Escape' || k === 'h' || k === 'H') help.classList.add('hidden');
        else if (k === 'PageDown') help.scrollTop += help.clientHeight * 0.9;
        else if (k === 'PageUp') help.scrollTop -= help.clientHeight * 0.9;
        else return;
        e.preventDefault(); return;
    }
    if ((tag === 'INPUT' && ['range', 'checkbox', 'color', 'button'].indexOf(ae.type) < 0) || tag === 'SELECT' || tag === 'TEXTAREA') {
        if (k === 'Escape') ae.blur();
        return;
    }
    if (E.xf) {
        const X = E.xf;
        if (k === 'x' || k === 'y' || k === 'z' || k === 'X' || k === 'Y' || k === 'Z') { const a = 'xyz'.indexOf(k.toLowerCase()); X.axis = X.axis === a ? -1 : a; xf_update(); }
        else if (k === 'Shift') { X.fine = true; X.fine_m0 = [E.mouse[0], E.mouse[1], 0]; }
        else if (k === 'Enter') xf_end(true);
        else if (k === 'Escape') xf_end(false);
        else return;
        e.preventDefault(); return;
    }
    const ctrl = e.ctrlKey || e.metaKey;
    if (ctrl && (k === 'z' || k === 'Z')) { if (e.shiftKey) do_redo(); else do_undo(); }
    else if (ctrl && (k === 'y' || k === 'Y')) do_redo();
    else if (ctrl) return;
    else if (k === 'g' || k === 'G') xf_start('grab');
    else if (k === 'r' || k === 'R') xf_start('rotate');
    else if (k === 's' || k === 'S') xf_start('scale');
    else if (k === 'n' || k === 'N') add_splat();
    else if ((k === 'd' || k === 'D') && e.shiftKey) duplicate();
    else if (k === 'Delete' || k === 'Backspace' || k === 'x' || k === 'X') delete_sel();
    else if ((k === 'a' || k === 'A') && e.altKey) { select_only([]); changed(true); }
    else if (k === 'a' || k === 'A') { select_only(E.list); changed(true); }
    else if (k === 'c' || k === 'C') { const h = pick(E.mouse[0], E.mouse[1]); E.cursor = h.p || ground_hit(); msg('cursor ' + E.cursor.map(v => v.toFixed(3)).join(' ')); }
    else if (k === 't' || k === 'T') apply_tool();
    else if (k === '1' || k === '2' || k === '3') { E.mode = parseInt(k, 10) - 1; }
    else if (k === 'l' || k === 'L') E.lit = !E.lit;
    else if (k === 'f' || k === 'F') frame_view();
    else if (k === 'h' || k === 'H') help.classList.remove('hidden');
    else return;
    e.preventDefault();
}

function ground_hit()
{
    const r = ray_at(E.mouse[0], E.mouse[1]);
    if (Math.abs(r.d[1]) < 1e-6) return E.cursor;
    const t = -r.o[1] / r.d[1];
    return t > 0 ? vmad(r.o, r.d, t) : E.cursor;
}

// ---------------------------------------------------------------------------------------------------------------
// Files

function download(name, data, mime)
{
    const url = URL.createObjectURL(new Blob([data], { type: mime || 'application/octet-stream' }));
    const a = el('a'); a.href = url; a.download = name; document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function load_file()
{
    const inp = el('input'); inp.type = 'file'; inp.accept = '.splats,.ply,.txt';
    inp.addEventListener('change', () => {
        const f = inp.files[0];
        if (!f) return;
        f.arrayBuffer().then(buf => {
            push_undo();
            if (/\.ply$/i.test(f.name)) E.list = ply_read(new Uint8Array(buf));
            else { const r = splats_read(new TextDecoder().decode(buf)); E.list = r.splats; if (r.errors.length) alert(r.errors.slice(0, 20).join('\n')); }
            E.title = f.name.replace(/\.[^.]*$/, ''); $('title').value = E.title;
            changed(true); frame_view(); msg('loaded ' + E.list.length + ' splats');
        }).catch(err => alert(String(err.message || err)));
    });
    inp.click();
}

function ui_init()
{
    $('btn_new').addEventListener('click', () => { if (!confirm('Start an empty model?')) return; push_undo(); E.list = []; changed(true); });
    $('btn_default').addEventListener('click', () => { if (!confirm('Replace the model with the example plant?')) return; push_undo(); E.list = default_model(); E.title = 'meadow_plant'; $('title').value = E.title; changed(true); frame_view(); });
    $('btn_load').addEventListener('click', load_file);
    $('btn_save').addEventListener('click', () => download(E.title + '.splats', splats_write(E.list, E.title), 'text/plain'));
    $('btn_ply').addEventListener('click', () => download(E.title + '.ply', ply_write(E.list)));
    $('btn_grid').addEventListener('click', () => { E.grid = !E.grid; });
    $('btn_help').addEventListener('click', () => { $('help').classList.toggle('hidden'); $('btn_help').blur(); });
    $('title').addEventListener('change', e => { E.title = e.target.value.trim().replace(/[^\w-]/g, '_') || 'model'; e.target.value = E.title; changed(false); });
    $('help_text').textContent = help_text();
    document.addEventListener('keydown', on_key);
    document.addEventListener('keyup', e => { if (e.key === 'Shift' && E.xf && E.xf.fine) { E.xf.fine = false; } });
}

function main()
{
    E.R = render_init($('view'));
    for (const id of Object.keys(TOOLS)) E.tool_P[id] = tool_defaults(id);
    let text = null;
    try { text = localStorage.getItem(STORE_MODEL); E.title = localStorage.getItem(STORE_TITLE) || E.title; } catch (e) { /* ignore */ }
    const r = text ? splats_read(text) : null;
    E.list = r && r.splats.length ? r.splats : default_model();
    $('title').value = E.title;
    ui_init(); view_input(); inspector_build(); tools_build();
    frame_view();
    const frame = function () {
        const cam = camera(), key = cam.view.map(v => v.toFixed(5)).join(',');
        if (E.data_dirty || key !== E.view_key) { render_upload(E.R, E.list, cam.view); E.view_key = key; E.data_dirty = false; }
        const nsel = E.list.reduce((n, t) => n + (t.sel ? 1 : 0), 0);
        render_frame(E.R, cam, { mode: E.mode, lit: E.lit, grid: E.grid, cursor: E.cursor, list: E.list, nsel: nsel, bg: [0.16, 0.17, 0.19] });
        let s = E.list.length + ' splats · ' + nsel + ' selected · sort ' + E.R.sort_ms.toFixed(1) + ' ms · view ' + ['splats', 'ellipsoids', 'centres'][E.mode] +
                (E.lit ? ' (lit)' : '') + ' · cursor ' + E.cursor.map(v => v.toFixed(3)).join(' ');
        if (E.xf) s += ' · ' + E.xf.mode + (E.xf.axis >= 0 ? ' ' + 'XYZ'[E.xf.axis] : '') + (E.xf.factor ? ' ×' + E.xf.factor.toFixed(3) : '') + (E.xf.angle ? ' ' + E.xf.angle.toFixed(1) + '°' : '');
        if (E.msg && performance.now() - E.msg_t < 4000) s += ' · ' + E.msg;
        $('status').textContent = s;
        keybar_update();
        requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
}

window.addEventListener('load', main);
