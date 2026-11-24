// Splat editor: hand-made Gaussian splat models. Selection, transforms (G/R/S like Blender), construction tools,
// inspector, undo, files (.splats text, INRIA .ply), autosave.

const STORE_MODEL = 'splat_editor.model', STORE_TITLE = 'splat_editor.title';
const FOV = 45 * Math.PI / 180;

const E = {
    list: [], title: 'meadow_plant', cursor: [0, 0, 0], mode: 0, edge: -1, slice_px: 20, sel_style: 2, outline_k: 1.2, lit: true, grid: true, centres: false,
    tool: 'chain', tool_P: {}, box: null, hover: -1, hover_cur: null, fading: [], mouse_in: false, dragging: false, mods: { shift: false, ctrl: false, alt: false }, undo: [], redo: [], xf: null, editing: false, mouse: [0, 0],
    cam: { az: 0.7, el: 0.35, dist: 1.2, target: [0, 0.12, 0] }, R: null, view_key: '', data_dirty: true, msg: '', msg_t: 0,
};

function $(id) { return document.getElementById(id); }
function sel_list() { return E.list.filter(t => t.sel); }
function msg(s) { E.msg = s; E.msg_t = performance.now(); }

// Large note at the bottom middle of the viewport that fades out over 2 s (e.g. the new splats presentation).
function toast(s)
{
    const d = $('toast'), c = E.R.canvas;
    d.textContent = s; d.style.left = (c.offsetLeft + c.offsetWidth / 2) + 'px';
    d.style.transition = 'none'; d.style.opacity = '1';
    void d.offsetWidth;                                                // restart the transition
    d.style.transition = 'opacity 2s linear'; d.style.opacity = '0';
}

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

// ---------------------------------------------------------------------------------------------------------------
// Box selection. LMB drag draws a rectangle. Modifiers, read live while dragging: Shift add, Ctrl remove (else replace);
// Alt: only splats completely inside the box (else every splat the box touches). A splat is its 2-sigma ellipse on
// screen (the EWA projection, as drawn). The same modifiers apply to a click: Shift toggles, Ctrl removes.

function sel_op(m) { return m.ctrl ? 'remove' : m.shift ? 'add' : 'replace'; }

// Screen ellipse of a splat in CSS pixels: centre and 2-sigma covariance (xx, xy, yy), or null when behind the camera.
function splat_ellipse(t, cam, F, rc)
{
    const v = cam.view, p = t.p;
    const vx = v[0] * p[0] + v[4] * p[1] + v[8] * p[2] + v[12], vy = v[1] * p[0] + v[5] * p[1] + v[9] * p[2] + v[13];
    const tz = -(v[2] * p[0] + v[6] * p[1] + v[10] * p[2] + v[14]);
    if (tz < 1e-3) return null;
    const ax = q_axes(t.q);
    let xx = 0.3, xy = 0, yy = 0.3;
    for (let k = 0; k < 3; k++) {
        const w = vscale(ax[k], t.s[k]), wx = v[0] * w[0] + v[4] * w[1] + v[8] * w[2], wy = v[1] * w[0] + v[5] * w[1] + v[9] * w[2];
        const wz = -(v[2] * w[0] + v[6] * w[1] + v[10] * w[2]);
        const jx = F / tz * wx + F * vx / (tz * tz) * wz, jy = -(F / tz * wy + F * vy / (tz * tz) * wz);   // screen y down
        xx += jx * jx; xy += jx * jy; yy += jy * jy;
    }
    const s = to_screen(p);
    return { x: s[0], y: s[1], xx: 4 * xx, xy: 4 * xy, yy: 4 * yy };
}

// Does the ellipse touch the box / lie completely inside it?
function ellipse_touches(e, b)
{
    if (e.x >= b.x0 && e.x <= b.x1 && e.y >= b.y0 && e.y <= b.y1) return true;
    const det = e.xx * e.yy - e.xy * e.xy; if (det <= 0) return false;
    const a = e.yy / det, c = e.xx / det, h = -e.xy / det;          // inverse covariance [[a, h], [h, c]]
    const q = (dx, dy) => a * dx * dx + 2 * h * dx * dy + c * dy * dy;
    // minimum of the quadratic form along each edge of the box (1D quadratic, clamped)
    for (const X of [b.x0, b.x1]) { const dx = X - e.x, dy = clamp(-h * dx / c, b.y0 - e.y, b.y1 - e.y); if (q(dx, dy) <= 1) return true; }
    for (const Y of [b.y0, b.y1]) { const dy = Y - e.y, dx = clamp(-h * dy / a, b.x0 - e.x, b.x1 - e.x); if (q(dx, dy) <= 1) return true; }
    return false;
}
function ellipse_inside(e, b)
{
    const ex = Math.sqrt(e.xx), ey = Math.sqrt(e.yy);
    return e.x - ex >= b.x0 && e.x + ex <= b.x1 && e.y - ey >= b.y0 && e.y + ey <= b.y1;
}

function box_apply(B, m)
{
    const cam = camera(), rc = E.R.canvas.getBoundingClientRect(), F = cam.focal * rc.height;
    const b = { x0: Math.min(B.x0, B.x1), x1: Math.max(B.x0, B.x1), y0: Math.min(B.y0, B.y1), y1: Math.max(B.y0, B.y1) };
    const hit = E.list.filter(function (t) { const e = splat_ellipse(t, cam, F, rc); return e && (m.alt ? ellipse_inside(e, b) : ellipse_touches(e, b)); });
    const op = sel_op(m);
    if (op === 'replace') select_only(hit);
    else for (const t of hit) t.sel = op === 'add';
    msg('box: ' + hit.length + ' splats ' + (op === 'remove' ? 'removed' : op === 'add' ? 'added' : 'selected') + (m.alt ? ' (enclosed)' : ' (touching)'));
    changed(true);
}

function box_overlay(B, m)
{
    const d = $('box');
    if (!B || !B.active) { d.classList.add('hidden'); return; }
    const rc = E.R.canvas.getBoundingClientRect();
    const x0 = clamp(Math.min(B.x0, B.x1), rc.left, rc.right), x1 = clamp(Math.max(B.x0, B.x1), rc.left, rc.right);
    const y0 = clamp(Math.min(B.y0, B.y1), rc.top, rc.bottom), y1 = clamp(Math.max(B.y0, B.y1), rc.top, rc.bottom);
    d.style.left = x0 + 'px'; d.style.top = y0 + 'px'; d.style.width = (x1 - x0) + 'px'; d.style.height = (y1 - y0) + 'px';
    d.className = 'box ' + sel_op(m) + (m.alt ? ' enclosed' : '');
}

// What a click on splat t would do with the current modifiers, and the hover colour for it.
function click_op(t) { const m = E.mods; return m.ctrl ? 'remove' : m.shift ? (t.sel ? 'remove' : 'add') : 'select'; }
const HOVER_FADE = 3000;
const HOVER_COL = { select: [0.45, 0.85, 1.0], add: [0.55, 0.95, 0.5], remove: [1.0, 0.45, 0.4] };

// Status line part describing selection: mode and the modifier keys with their effect (held ones highlighted).
function sel_status()
{
    const m = E.mods, on = (b, s) => b ? '<b class="on">' + s + '</b>' : s;
    const head = E.box && E.box.active ? 'BOX SELECT' : 'select: click / drag box';
    return '<b>' + head + '</b> ' + sel_op(m) + ', ' + (m.alt ? 'enclosed only' : 'touching') +
           ' [' + on(m.shift, 'Shift add') + ' · ' + on(m.ctrl, 'Ctrl remove') + ' · ' + on(m.alt, 'Alt enclosed only') + ']' +
           (E.hover >= 0 && E.list[E.hover] ? ' · click: ' + click_op(E.list[E.hover]) + ' #' + E.hover : '');
}

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

// Edge mode of the selected splat(s).
function mode_select(value, set)
{
    const row = el('div', 'crow'), sel = el('select');
    row.appendChild(el('span', 'pname', 'edge rendering mode'));
    EDGE_NAMES.forEach((n, i) => { const o = el('option', null, n); o.value = i; sel.appendChild(o); });
    sel.value = value;
    sel.addEventListener('change', () => { set(parseInt(sel.value, 10)); sel.blur(); });
    row.appendChild(sel);
    return row;
}

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
                           ui_bar('opacity', { min: 0, max: 1, step: 0.01, get: () => t.a, set: (v, f) => { edit(f); t.a = v; changed(false); } }),
                           ui_bar('hardness', { min: 0, max: 1, step: 0.01, get: () => t.h, set: (v, f) => { edit(f); t.h = v; changed(false); } }),
                           mode_select(t.m, v => { edit(true); t.m = v; changed(false); })]);
    } else {
        const g = el('div', 'pgroup');
        g.appendChild(el('div', 'gtitle', 'all selected'));
        g.appendChild(ui_color('colour', () => rgb_hex(s[0].c), (v, f) => set_all(t => { t.c = hex_rgb(v); }, f)));
        g.appendChild(ui_bar('opacity', { min: 0, max: 1, step: 0.01, get: () => s[0].a, set: (v, f) => set_all(t => { t.a = v; }, f) }));
        g.appendChild(ui_bar('hardness', { min: 0, max: 1, step: 0.01, get: () => s[0].h, set: (v, f) => set_all(t => { t.h = v; }, f) }));
        g.appendChild(mode_select(s[0].m, v => set_all(t => { t.m = v; }, true)));
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

const EDGE_NAMES = ['Gaussian', 'saturated', 'generalized', 'cut-off'];

const KEYBAR = {
    edit: ['EDIT', ['LMB', 'select / drag box'], ['G R S', 'move / rotate / scale'], ['N', 'new splat'], ['Shift+D', 'duplicate'], ['Del', 'delete'],
           ['A', 'select all'], ['Alt+A', 'none'], ['C', 'cursor to mouse'], ['T', 'apply tool'], ['1 2 3', 'splats presentation'],
           ['L', 'lighting'], ['E', 'edge rendering mode'], ['V', 'view settings'], ['F', 'frame'], ['Ctrl+Z / Y', 'undo / redo'], ['RMB', 'orbit'], ['MMB', 'pan'], ['Wheel', 'zoom'], ['H', 'help']],
    xf: ['TRANSFORM', ['X Y Z', 'axis (scale: local axis)'], ['Shift', 'fine'], ['LMB / Enter', 'confirm'], ['RMB / Esc', 'cancel']],
    help: ['HELP', ['PgUp/PgDn', 'scroll'], ['Esc', 'close']],
    box: ['BOX SELECT', ['Shift', 'add'], ['Ctrl', 'remove'], ['Alt', 'only enclosed splats'], ['release', 'apply'], ['Esc', 'cancel']],
};

function keybar_update()
{
    const mode = !$('help').classList.contains('hidden') ? 'help' : E.xf ? 'xf' : E.box && E.box.active ? 'box' : 'edit';
    const key = mode + (E.xf ? E.xf.mode + E.xf.axis : '');
    if (E.keybar_key === key && E.keybar_w === window.innerWidth) return;
    E.keybar_w = window.innerWidth;
    E.keybar_key = key;
    const k = KEYBAR[mode], bar = $('keybar');
    bar.innerHTML = keybar_html(k, E.xf ? E.xf.mode.toUpperCase() + (E.xf.axis >= 0 ? ' ' + 'XYZ'[E.xf.axis] : '') : k[0]);
    // The bar keeps the height of the (tallest) edit page whatever page it shows, so the viewport never resizes when a
    // transform or a box selection starts. Measured on a hidden copy at the bar's width.
    const probe = document.createElement('div');
    probe.id = 'keybar'; probe.style.cssText = 'position:absolute; visibility:hidden; left:0; top:0; width:' + bar.offsetWidth + 'px; min-height:0';
    probe.innerHTML = keybar_html(KEYBAR.edit, KEYBAR.edit[0]);
    document.body.appendChild(probe); const hh = probe.offsetHeight; probe.remove();
    bar.style.minHeight = bar.style.height = hh + 'px';
}
function keybar_html(k, title)
{
    let h = '<span class="slot mode">' + title + '</span>';
    for (const e of k.slice(1)) h += '<span class="slot"><kbd>' + e[0] + '</kbd>' + e[1] + '</span>';
    return h;
}

function help_text()
{
    return 'SPLAT EDITOR - hand-made Gaussian splat models (no capture data)\n\n' +
        'A splat is a 3D Gaussian: position, three standard deviations (sigma) along its own axes, a rotation, colour and opacity.\n' +
        'It is drawn as a soft ellipse facing the camera; the visible edge is at about 2 sigma. A flat splat (one sigma small)\n' +
        'is a leaf patch, a long thin one a blade or needle segment, a round one a blob.\n\n' +
        'SELECT   LMB on a splat (Shift: toggle, Ctrl: remove); LMB on empty space: none; A all, Alt+A none\n' +
        '         LMB drag: box. Every splat the box touches (its 2-sigma ellipse); hold Alt: only splats completely\n' +
        '         inside. Shift adds, Ctrl removes, otherwise the box replaces the selection. Esc cancels.\n' +
        'MOVE     G, then mouse; X/Y/Z lock to a world axis; Shift fine; LMB/Enter confirm, RMB/Esc cancel\n' +
        'ROTATE   R, then mouse around the selection centre; X/Y/Z: around a world axis, else the view axis\n' +
        'SCALE    S, then mouse away from / towards the centre; X/Y/Z: only that local sigma, else all (and positions)\n' +
        'CREATE   N: one splat at the 3D cursor; Shift+D: duplicate and move; Del: delete\n' +
        'CURSOR   C: put the 3D cursor where the mouse points (on a splat, else on the ground plane)\n' +
        'TOOLS    panel on the right: Chain (blade, stem), Leaf, Blob build at the cursor; Radial copies, Mirror and Colour\n' +
        '         variation work on the selection with the cursor as pivot. T applies the current tool.\n' +
        'VIEW     RMB drag orbit, MMB (or Shift+RMB) drag pan, wheel zoom, F frame selection (or all)\n' +
        '         1 splats, 2 contours (sections of the 2-sigma ellipsoid parallel to the splat\'s own planes: the central one,\n' +
        '         then every "Slice distance" pixels (view settings, V) out to the edge; planes with an axis under 3 px hidden). The selection\n' +
        '         is always drawn this way, in orange.\n         3 centres; L lighting (normal = shortest axis); grid in the view settings (V)\n' +
        'EDGE     each splat has an edge mode and a "hardness" (0..1), both in the panel. The view settings list (V) or E shows\n' +
        '         each splat in its own mode ("per splat") or forces one mode on all. Compare: four copies of the model\n' +
        '         side by side (Gaussian, saturated, generalized, cut-off), each adapted to its mode.\n' +
        '         Gaussian: hardness ignored. Saturated: opacity x (1 + 7 h), clamped: flat top, steeper rim.\n' +
        '         Generalized: exp(-2 (r/2)^(2 beta)), beta = 2^(4 h) = 1..16: flat top, sharp rim at 2 sigma.\n' +
        '         Cut-off: solid inside the 2-sigma contour, rim width (1 - h); at least ~1 px everywhere.\n' +
        'SETTINGS V: view settings (rendering, edge mode, lighting, grid, selection display, outline, slice distance)\n' +
        'UNDO     Ctrl+Z, Ctrl+Y (or Ctrl+Shift+Z)\n' +
        'FILES    .splats: one splat per line "x y z  sx sy sz  qw qx qy qz  #rrggbb  opacity  hardness  mode" (metres, sigma)\n' +
        '         .ply: INRIA 3D Gaussian Splatting format (SH degree 0), opens in SuperSplat / SIBR; import reads it back\n';
}

// ---------------------------------------------------------------------------------------------------------------
// Input

function view_input()
{
    const c = E.R.canvas;
    let drag = null;
    c.addEventListener('contextmenu', e => e.preventDefault());
    c.addEventListener('mouseenter', () => { E.mouse_in = true; });
    c.addEventListener('mouseleave', () => { E.mouse_in = false; });
    const mods = e => { E.mods = { shift: e.shiftKey, ctrl: e.ctrlKey || e.metaKey, alt: e.altKey }; if (E.box) box_overlay(E.box, E.mods); };
    window.addEventListener('keydown', e => { mods(e); if (e.key === 'Alt') e.preventDefault(); });
    window.addEventListener('keyup', e => { mods(e); if (e.key === 'Alt') e.preventDefault(); });
    window.addEventListener('blur', () => { E.mods = { shift: false, ctrl: false, alt: false }; });
    c.addEventListener('mousedown', e => {
        c.focus(); mods(e);
        E.mouse = [e.clientX, e.clientY];
        if (E.xf) { xf_end(e.button === 0); e.preventDefault(); return; }
        if (e.button === 0) {                                         // click selects on release; a drag draws a box
            E.box = { x0: e.clientX, y0: e.clientY, x1: e.clientX, y1: e.clientY, active: false };
            e.preventDefault();
            return;
        }
        drag = { x: e.clientX, y: e.clientY, pan: e.button === 1 || e.shiftKey }; E.dragging = true;
        e.preventDefault();
    });
    window.addEventListener('mouseup', e => {
        drag = null; E.dragging = false;
        if (e.button !== 0 || !E.box) return;
        mods(e);
        const B = E.box; E.box = null; box_overlay(null);
        if (B.active) { box_apply(B, E.mods); return; }
        const h = pick(B.x0, B.y0), m = E.mods;
        if (h.i >= 0) { const t = E.list[h.i]; if (m.ctrl) t.sel = false; else if (m.shift) t.sel = !t.sel; else select_only([t]); }
        else if (!m.shift && !m.ctrl) select_only([]);
        changed(true);
    });
    window.addEventListener('mousemove', e => {
        E.mouse = [e.clientX, e.clientY]; mods(e);
        if (E.xf) { xf_update(); return; }
        if (E.box) {
            E.box.x1 = e.clientX; E.box.y1 = e.clientY;
            if (Math.abs(E.box.x1 - E.box.x0) + Math.abs(E.box.y1 - E.box.y0) > 4) E.box.active = true;
            box_overlay(E.box, E.mods);
            return;
        }
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
    if (k === 'Escape' && !$('viewset').classList.contains('hidden') && !E.box && !E.xf) { $('viewset').classList.add('hidden'); if (ae) ae.blur(); e.preventDefault(); return; }
    if (E.box && k === 'Escape') { E.box = null; box_overlay(null); msg('box select cancelled'); e.preventDefault(); return; }
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
    else if (k === '1' || k === '2' || k === '3') { E.mode = parseInt(k, 10) - 1; toast(k + ' · ' + ['splats', 'contours', 'centres'][E.mode]); }
    else if (k === 'v' || k === 'V') $('viewset').classList.toggle('hidden');
    else if (k === 'l' || k === 'L') E.lit = !E.lit;
    else if (k === 'e' || k === 'E') { E.edge = E.edge >= 3 ? -1 : E.edge + 1; $('edge').value = E.edge; toast('E · edge: ' + (E.edge < 0 ? 'per splat' : 'all ' + EDGE_NAMES[E.edge])); }
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
    $('btn_compare').addEventListener('click', () => {
        if (!E.list.length) return msg('nothing to compare');
        if (!confirm('Replace the model with four copies side by side, one per edge mode (Gaussian, saturated, generalized, cut-off)?')) return;
        push_undo(); E.list = compare_layout(E.list); E.edge = -1; $('edge').value = -1; select_only([]); changed(true); frame_view();
    });
    $('btn_default').addEventListener('click', () => { if (!confirm('Replace the model with the example plant?')) return; push_undo(); E.list = default_model(); E.title = 'meadow_plant'; $('title').value = E.title; changed(true); frame_view(); });
    $('btn_load').addEventListener('click', load_file);
    $('btn_save').addEventListener('click', () => download(E.title + '.splats', splats_write(E.list, E.title), 'text/plain'));
    $('btn_ply').addEventListener('click', () => download(E.title + '.ply', ply_write(E.list)));
    $('btn_grid').addEventListener('click', () => { E.grid = !E.grid; });
    $('btn_lit').addEventListener('click', () => { E.lit = !E.lit; });
    const sl = $('slice_px'), slv = () => { E.slice_px = parseInt(sl.value, 10); $('slice_val').textContent = E.slice_px + ' px'; };
    sl.addEventListener('input', slv); sl.addEventListener('change', () => sl.blur()); slv();
    const ok = $('outline_k'), okv = () => { E.outline_k = parseFloat(ok.value); $('outline_val').textContent = E.outline_k.toFixed(1) + ' σ'; };
    ok.addEventListener('input', okv); ok.addEventListener('change', () => ok.blur()); okv();
    $('view_mode').addEventListener('change', ev => { E.mode = parseInt(ev.target.value, 10); ev.target.blur(); });
    $('sel_style').addEventListener('change', ev => { E.sel_style = parseInt(ev.target.value, 10); ev.target.blur(); });
    $('edge').addEventListener('change', ev => { E.edge = parseInt(ev.target.value, 10); ev.target.blur(); });
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
        // Outline colour: average colour of the selected splats, brightened so the line reads on the dark background.
        let oc = [0, 0, 0];
        if (nsel) { for (const t of E.list) if (t.sel) oc = vadd(oc, t.c); oc = vscale(oc, 1 / nsel); const mx = Math.max(...oc, 1e-3); oc = oc.map(x => Math.min(1, x * Math.max(1.25, 0.95 / mx) + 0.05)); }
        // hover: the splat a click at the mouse would pick (not while transforming, box selecting or orbiting)
        E.hover = E.mouse_in && !E.xf && !E.box && !E.dragging && $('help').classList.contains('hidden') ? pick(E.mouse[0], E.mouse[1]).i : -1;
        const hv = E.hover >= 0 ? E.list[E.hover] : null;
        // The highlight is constant while the mouse is over the splat; when the mouse leaves an unselected splat (or a
        // modifier changes the click's effect), its highlight fades out over HOVER_FADE ms. Selected splats go straight
        // back to orange.
        const now = performance.now(), hcol = hv ? HOVER_COL[click_op(hv)] : null;
        if (E.hover_cur && !E.hover_cur.t.sel && (E.hover_cur.t !== hv || E.hover_cur.col !== hcol)) E.fading.push({ t: E.hover_cur.t, col: E.hover_cur.col, t0: now });
        E.hover_cur = hv ? { t: hv, col: hcol } : null;
        E.fading = E.fading.filter(f => now - f.t0 < HOVER_FADE && f.t !== hv && !f.t.sel && E.list.indexOf(f.t) >= 0);
        const hover = E.fading.map(f => ({ t: f.t, col: f.col, a: 1 - (now - f.t0) / HOVER_FADE }));
        if (hv) hover.push({ t: hv, col: hcol, a: 1 });
        $('btn_grid').classList.toggle('on', E.grid); $('btn_lit').classList.toggle('on', E.lit);   // toggle state (also changed by keys)
        if ($('view_mode').value !== '' + E.mode && document.activeElement !== $('view_mode')) $('view_mode').value = E.mode;
        render_frame(E.R, cam, { mode: E.mode, edge: E.edge, slice_px: E.slice_px, sel_style: E.sel_style, outline_col: oc, outline_k: E.outline_k, lit: E.lit, grid: E.grid, cursor: E.cursor, list: E.list, nsel: nsel, hover: hover, bg: [0.16, 0.17, 0.19] });
        let s = E.list.length + ' splats · ' + nsel + ' selected · sort ' + E.R.sort_ms.toFixed(1) + ' ms · view ' + ['splats', 'contours', 'centres'][E.mode] +
                (E.lit ? ' (lit)' : '') + ' · edge ' + (E.edge < 0 ? 'per splat' : EDGE_NAMES[E.edge] + ' (all)') + ' · cursor ' + E.cursor.map(v => v.toFixed(3)).join(' ');
        if (E.xf) s += ' · ' + E.xf.mode + (E.xf.axis >= 0 ? ' ' + 'XYZ'[E.xf.axis] : '') + (E.xf.factor ? ' ×' + E.xf.factor.toFixed(3) : '') + (E.xf.angle ? ' ' + E.xf.angle.toFixed(1) + '°' : '');
        if (E.msg && performance.now() - E.msg_t < 4000) s += ' · ' + E.msg;
        const esc = x => x.replace(/&/g, '&amp;').replace(/</g, '&lt;');
        $('status').innerHTML = sel_status() + ' · ' + esc(s);
        keybar_update();
        requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
}

window.addEventListener('load', main);
