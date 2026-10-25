// Splat model, construction tools, files.
//
// Splat = { p: [x, y, z] (m), s: [sx, sy, sz] (standard deviations along the local axes, m), q: [w, x, y, z] (rotation),
//           c: [r, g, b] (sRGB 0..1), a: opacity 0..1, sel: bool }.
// The Gaussian is G(x) = a * exp(-1/2 (x-p)^T (R S S^T R^T)^-1 (x-p)); the visible edge is at ~2 sigma.

function splat_make(p, s, q, c, a) { return { p: p.slice(), s: s.slice(), q: q.slice(), c: c.slice(), a: a, sel: false }; }
function splat_clone(t) { return splat_make(t.p, t.s, t.q, t.c, t.a); }

function hex_rgb(h) { const v = parseInt(h.slice(1), 16); return [(v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255]; }
function rgb_hex(c) { return '#' + c.map(x => ('0' + Math.round(clamp(x, 0, 1) * 255).toString(16)).slice(-2)).join(''); }

// ---------------------------------------------------------------------------------------------------------------
// Construction tools. Each returns new splats; they never modify the input.

// Splat aligned to an orthonormal frame: x = along (tangent), y = across (side), z = normal (thin axis).
function splat_frame(p, t, side, s, c, a)
{
    const n = vnorm(vcross(t, side));
    return splat_make(p, s, q_from_axes(t, vnorm(vcross(n, t)), n), c, a);
}

// Blade, stem or twig: a chain of elongated splats along a quadratic Bezier curve from base b.
// P: length, bend (droop towards the tip), azimuth, tilt (deg from vertical), count, width, thickness, taper,
//    color0 (base), color1 (tip), opacity.
function tool_chain(b, P)
{
    const az = P.azimuth * Math.PI / 180, tl = P.tilt * Math.PI / 180;
    const dir = [Math.sin(tl) * Math.cos(az), Math.cos(tl), Math.sin(tl) * Math.sin(az)];
    const hor = vnorm([Math.cos(az), 0, Math.sin(az)]);
    const p1 = vmad(b, dir, P.length * 0.55);
    const p2 = vadd(vmad(b, dir, P.length * (1 - 0.3 * P.bend)), vadd(vscale(hor, P.bend * P.length * 0.45), [0, -P.bend * P.length * 0.5, 0]));
    const bez = t => vadd(vadd(vscale(b, (1 - t) * (1 - t)), vscale(p1, 2 * t * (1 - t))), vscale(p2, t * t));
    const dbez = t => vnorm(vadd(vscale(vsub(p1, b), 2 * (1 - t)), vscale(vsub(p2, p1), 2 * t)));
    const out = [], n = Math.max(1, Math.round(P.count)), c0 = hex_rgb(P.color0), c1 = hex_rgb(P.color1);
    let len = 0;
    for (let i = 0; i < 16; i++) len += vlen(vsub(bez((i + 1) / 16), bez(i / 16)));
    const side = [-Math.sin(az), 0, Math.cos(az)];      // the blade's flat side faces along the azimuth
    for (let i = 0; i < n; i++) {
        const t = (i + 0.5) / n, tg = dbez(t);
        const w = Math.max(P.width * (1 - P.taper * t), 1e-4);
        out.push(splat_frame(bez(t), tg, side, [len / n * 0.6, w, P.thickness], vlerp(c0, c1, t), P.opacity));
    }
    return out;
}

// Flat leaf: a row of flat splats along the midrib, widths following an ovate outline.
// P: length, width, rows, azimuth, tilt (deg from vertical), curl (droop), color0, color1, opacity.
function tool_leaf(b, P)
{
    const az = P.azimuth * Math.PI / 180, tl = P.tilt * Math.PI / 180;
    const dir = [Math.sin(tl) * Math.cos(az), Math.cos(tl), Math.sin(tl) * Math.sin(az)];
    const side = vnorm([-Math.sin(az), 0, Math.cos(az)]);
    const n = Math.max(1, Math.round(P.rows)), c0 = hex_rgb(P.color0), c1 = hex_rgb(P.color1), out = [];
    let p = b.slice(), d = dir;
    const step = P.length / n;
    for (let i = 0; i < n; i++) {
        const t = (i + 0.5) / n;
        d = vnorm(vadd(d, [0, -P.curl * 2 / n, 0]));
        const c = vmad(p, d, step * 0.5);
        const hw = P.width * 0.5 * Math.pow(Math.sin(Math.PI * Math.pow(t, 0.75)), 0.8);
        out.push(splat_frame(c, d, side, [step * 0.62, Math.max(hw * 0.62, 1e-4), Math.max(P.width * 0.02, 3e-4)], vlerp(c0, c1, t), P.opacity));
        p = vmad(p, d, step);
    }
    return out;
}

// Round blob of n splats (flower head, berry, bud): random positions inside a sphere, flat facing outwards.
function tool_blob(b, P)
{
    const r = rng_make(P.seed), n = Math.max(1, Math.round(P.count)), out = [], c0 = hex_rgb(P.color0), c1 = hex_rgb(P.color1);
    for (let i = 0; i < n; i++) {
        let v;
        do v = [rng_s(r), rng_s(r), rng_s(r)]; while (vdot(v, v) > 1);
        const nrm = vlen(v) > 1e-3 ? vnorm(v) : [0, 1, 0];
        const t = vperp(nrm);
        out.push(splat_frame(vmad(b, v, P.radius), t, vcross(nrm, t), [P.size, P.size, P.size * P.flatness], vlerp(c0, c1, rng_f(r)), P.opacity));
    }
    return out;
}

// Copies of the given splats rotated around the vertical axis through pivot (count includes the original),
// with seeded jitter of angle, scale and tilt.
function tool_radial(src, pivot, P)
{
    const r = rng_make(P.seed), n = Math.max(1, Math.round(P.count)), out = [];
    for (let k = 0; k < n; k++) {
        const a = k * 2 * Math.PI / n + rng_s(r) * P.angle_jitter * Math.PI / 180;
        const sc = 1 + rng_s(r) * P.scale_jitter, tilt = rng_s(r) * P.tilt_jitter * Math.PI / 180;
        const qy = q_axis([0, 1, 0], a), qt = q_axis(q_rotate(qy, [0, 0, 1]), tilt), qr = q_mul(qt, qy);
        for (const t of src) {
            const c = splat_clone(t);
            c.p = vadd(pivot, vscale(q_rotate(qr, vsub(t.p, pivot)), sc));
            c.q = q_norm(q_mul(qr, t.q));
            c.s = vscale(t.s, sc);
            out.push(c);
        }
    }
    return out;
}

// Mirror copies across the plane x = pivot[0].
function tool_mirror(src, pivot)
{
    return src.map(t => {
        const c = splat_clone(t);
        c.p = [2 * pivot[0] - t.p[0], t.p[1], t.p[2]];
        const ax = q_axes(t.q);    // reflect the axes; flip one to stay right-handed
        const f = v => [-v[0], v[1], v[2]];
        c.q = q_from_axes(f(ax[0]), vscale(f(ax[1]), -1), vcross(f(ax[0]), vscale(f(ax[1]), -1)));
        return c;
    });
}

// Seeded variation of colour (brightness, hue towards yellow/red) and opacity.
function tool_jitter_color(src, P)
{
    const r = rng_make(P.seed);
    return src.map(t => {
        const c = splat_clone(t), br = 1 + rng_s(r) * P.brightness, hu = rng_s(r) * P.hue;
        c.c = [clamp(t.c[0] * br * (1 + hu), 0, 1), clamp(t.c[1] * br, 0, 1), clamp(t.c[2] * br * (1 - hu), 0, 1)];
        return c;
    });
}

const TOOLS = {
    chain: { name: 'Chain (blade, stem)', uses: 'cursor', fn: tool_chain, params: [
        ['length', 0.01, 3, 0.35, 0.01], ['bend', 0, 1.5, 0.45, 0.01], ['azimuth', -180, 180, 0, 1], ['tilt', 0, 90, 18, 1],
        ['count', 1, 40, 9, 1], ['width', 0.0005, 0.05, 0.006, 0.0005], ['thickness', 0.0002, 0.02, 0.0012, 0.0001],
        ['taper', 0, 1, 0.75, 0.01], ['color0', '#3a5a20'], ['color1', '#9ab44e'], ['opacity', 0.05, 1, 0.95, 0.01]] },
    leaf: { name: 'Leaf', uses: 'cursor', fn: tool_leaf, params: [
        ['length', 0.01, 1, 0.14, 0.005], ['width', 0.005, 0.5, 0.05, 0.005], ['rows', 1, 20, 6, 1], ['azimuth', -180, 180, 0, 1],
        ['tilt', 0, 180, 70, 1], ['curl', -1, 1, 0.25, 0.01], ['color0', '#3d6a22'], ['color1', '#5f8a30'], ['opacity', 0.05, 1, 0.97, 0.01]] },
    blob: { name: 'Blob (flower head, bud)', uses: 'cursor', fn: tool_blob, params: [
        ['radius', 0.002, 0.5, 0.018, 0.001], ['count', 1, 200, 24, 1], ['size', 0.0005, 0.1, 0.006, 0.0005], ['flatness', 0.05, 1, 0.35, 0.01],
        ['color0', '#f2d22e'], ['color1', '#e8a91c'], ['opacity', 0.05, 1, 0.95, 0.01], ['seed', 0, 9999, 1, 1]] },
    radial: { name: 'Radial copies of selection', uses: 'selection', fn: null, params: [
        ['count', 1, 24, 6, 1], ['angle_jitter', 0, 90, 12, 1], ['scale_jitter', 0, 0.8, 0.15, 0.01], ['tilt_jitter', 0, 45, 8, 1], ['seed', 0, 9999, 1, 1]] },
    mirror: { name: 'Mirror selection (x)', uses: 'selection', fn: null, params: [] },
    jitter: { name: 'Colour variation of selection', uses: 'selection', fn: null, params: [
        ['brightness', 0, 0.6, 0.15, 0.01], ['hue', 0, 0.4, 0.06, 0.01], ['seed', 0, 9999, 1, 1]] },
};

function tool_defaults(id)
{
    const P = {};
    for (const d of TOOLS[id].params) P[d[0]] = d[3] !== undefined ? d[3] : d[1];
    return P;
}

// ---------------------------------------------------------------------------------------------------------------
// Files. .splats text: one splat per line "x y z  sx sy sz  qw qx qy qz  #rrggbb  opacity"; '#' comments.

function fmt(x) { return (Math.round(x * 1e5) / 1e5).toString(); }

function splats_write(list, title)
{
    const L = ['# ' + (title || 'model') + ' - splat editor model (hand-made Gaussian splats)',
               '# x y z  sx sy sz (sigma, m)  qw qx qy qz  colour  opacity'];
    for (const t of list) L.push([t.p.map(fmt).join(' '), t.s.map(fmt).join(' '), t.q.map(fmt).join(' '), rgb_hex(t.c), fmt(t.a)].join('  '));
    return L.join('\n') + '\n';
}

function splats_read(text)
{
    const out = [], errors = [];
    text.split(/\r?\n/).forEach(function (line, i) {
        line = line.replace(/^\s*#(?![0-9a-fA-F]{6}\b).*$/, '').trim();
        if (!line) return;
        const tok = line.split(/\s+/);
        if (tok.length !== 12) { errors.push('line ' + (i + 1) + ': expected 12 fields'); return; }
        const v = tok.map((x, k) => k === 10 ? x : parseFloat(x));
        if (!/^#[0-9a-fA-F]{6}$/.test(tok[10]) || v.some((x, k) => k !== 10 && isNaN(x))) { errors.push('line ' + (i + 1) + ': bad value'); return; }
        out.push(splat_make(v.slice(0, 3), v.slice(3, 6).map(x => Math.max(x, 1e-5)), q_norm(v.slice(6, 10)), hex_rgb(tok[10]), clamp(v[11], 0, 1)));
    });
    return { splats: out, errors: errors };
}

// INRIA 3DGS .ply (binary little endian): position, normal (unused, 0), SH degree 0 colour, opacity as logit,
// scales as log, rotation (w, x, y, z). Opens in SuperSplat, SIBR and the usual 3DGS viewers.
const SH_C0 = 0.28209479177387814;

function ply_write(list)
{
    const props = ['x', 'y', 'z', 'nx', 'ny', 'nz', 'f_dc_0', 'f_dc_1', 'f_dc_2', 'opacity', 'scale_0', 'scale_1', 'scale_2', 'rot_0', 'rot_1', 'rot_2', 'rot_3'];
    const head = 'ply\nformat binary_little_endian 1.0\ncomment hand-made splats (plant editor project, splat editor)\nelement vertex ' + list.length + '\n' +
                 props.map(p => 'property float ' + p).join('\n') + '\nend_header\n';
    const hb = new TextEncoder().encode(head), body = new Float32Array(list.length * props.length);
    list.forEach(function (t, i) {
        const a = clamp(t.a, 1e-4, 1 - 1e-4);
        body.set([t.p[0], t.p[1], t.p[2], 0, 0, 0, (t.c[0] - 0.5) / SH_C0, (t.c[1] - 0.5) / SH_C0, (t.c[2] - 0.5) / SH_C0,
                  Math.log(a / (1 - a)), Math.log(t.s[0]), Math.log(t.s[1]), Math.log(t.s[2]), t.q[0], t.q[1], t.q[2], t.q[3]], i * props.length);
    });
    const out = new Uint8Array(hb.length + body.byteLength);
    out.set(hb); out.set(new Uint8Array(body.buffer), hb.length);
    return out;
}

// Reads binary little-endian .ply files with the INRIA property names (float properties; others skipped by size).
function ply_read(bytes)
{
    const txt = new TextDecoder().decode(bytes.subarray(0, Math.min(bytes.length, 65536)));
    const end = txt.indexOf('end_header\n');
    if (!txt.startsWith('ply') || end < 0) throw new Error('not a PLY file');
    if (txt.indexOf('binary_little_endian') < 0) throw new Error('only binary little-endian PLY is supported');
    const lines = txt.slice(0, end).split('\n'), props = [];
    let count = 0, in_vertex = false;
    const size = { float: 4, float32: 4, double: 8, uchar: 1, uint8: 1, char: 1, int8: 1, short: 2, ushort: 2, int: 4, uint: 4, int32: 4, uint32: 4 };
    for (const l of lines) {
        const t = l.trim().split(/\s+/);
        if (t[0] === 'element') { in_vertex = t[1] === 'vertex'; if (in_vertex) count = parseInt(t[2], 10); }
        else if (t[0] === 'property' && in_vertex) props.push({ type: t[1], name: t[2], size: size[t[1]] || 4 });
    }
    const stride = props.reduce((s, p) => s + p.size, 0), dv = new DataView(bytes.buffer, bytes.byteOffset + end + 11);
    const off = {};
    let o = 0;
    for (const p of props) { off[p.name] = { o: o, type: p.type }; o += p.size; }
    const get = (i, n) => { const f = off[n]; if (!f) return 0; const at = i * stride + f.o; return f.type === 'double' ? dv.getFloat64(at, true) : f.type === 'uchar' || f.type === 'uint8' ? dv.getUint8(at) / 255 : dv.getFloat32(at, true); };
    const out = [];
    for (let i = 0; i < count; i++) {
        const c = off.f_dc_0 ? [0.5 + SH_C0 * get(i, 'f_dc_0'), 0.5 + SH_C0 * get(i, 'f_dc_1'), 0.5 + SH_C0 * get(i, 'f_dc_2')]
                             : [get(i, 'red'), get(i, 'green'), get(i, 'blue')];
        const op = off.opacity ? 1 / (1 + Math.exp(-get(i, 'opacity'))) : 1;
        const s = off.scale_0 ? [Math.exp(get(i, 'scale_0')), Math.exp(get(i, 'scale_1')), Math.exp(get(i, 'scale_2'))] : [0.01, 0.01, 0.01];
        const q = off.rot_0 ? q_norm([get(i, 'rot_0'), get(i, 'rot_1'), get(i, 'rot_2'), get(i, 'rot_3')]) : q_ident();
        out.push(splat_make([get(i, 'x'), get(i, 'y'), get(i, 'z')], s, q, c.map(x => clamp(x, 0, 1)), op));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Default model: a small meadow plant made only with the tools above (tuft of grass blades, a dandelion-like leaf
// rosette, one stalk with a flower head).

function default_model()
{
    const out = [], o = [0, 0, 0];
    // Grass tuft: one blade, radial copies.
    let blade = tool_chain(o, Object.assign(tool_defaults('chain'), { length: 0.32, tilt: 14, azimuth: 0 }));
    for (const t of tool_radial(blade, o, Object.assign(tool_defaults('radial'), { count: 9, seed: 3, scale_jitter: 0.25, tilt_jitter: 12 }))) out.push(t);
    blade = tool_chain(o, Object.assign(tool_defaults('chain'), { length: 0.22, tilt: 35, azimuth: 20, bend: 0.8 }));
    for (const t of tool_radial(blade, o, Object.assign(tool_defaults('radial'), { count: 6, seed: 7, scale_jitter: 0.3 }))) out.push(t);
    // Leaf rosette next to it.
    const r0 = [0.16, 0, 0.05];
    const leaf = tool_leaf(r0, Object.assign(tool_defaults('leaf'), { tilt: 78, curl: 0.15 }));
    for (const t of tool_radial(leaf, r0, Object.assign(tool_defaults('radial'), { count: 7, seed: 5 }))) out.push(t);
    // Flower stalk and head.
    const stalk = tool_chain(r0, Object.assign(tool_defaults('chain'), { length: 0.24, tilt: 6, bend: 0.1, count: 8, width: 0.0022, thickness: 0.0018,
                                                                         taper: 0.2, color0: '#4c6b2a', color1: '#6b8a3a' }));
    for (const t of stalk) out.push(t);
    const top = vmad(stalk[stalk.length - 1].p, q_rotate(stalk[stalk.length - 1].q, [1, 0, 0]), stalk[stalk.length - 1].s[0] * 1.5);
    for (const t of tool_blob(top, tool_defaults('blob'))) out.push(t);
    return tool_jitter_color(out, Object.assign(tool_defaults('jitter'), { seed: 11 }));
}
