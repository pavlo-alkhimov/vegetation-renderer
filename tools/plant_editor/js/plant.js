// Plant structure and geometry: skeleton of branches (polylines with radii), branching, tropism, pruning,
// tube meshing of the wood, leaf organ templates, leaf placement and leaf meshing.
//
// Skeleton = { branches: [Branch], height }; Branch = { pts: [[x,y,z]], rad: [r], order, parent, t, dead }.
// Branches are stored in creation order, so a parent always precedes its children; t is the attachment position
// along the parent as a fraction of its arc length. Operators never modify their inputs (results are cached).

function skel_clone(sk)
{
    const out = { branches: new Array(sk.branches.length), height: sk.height };
    for (let i = 0; i < sk.branches.length; i++) {
        const b = sk.branches[i];
        out.branches[i] = { pts: b.pts.map(p => p.slice()), rad: b.rad.slice(), order: b.order, parent: b.parent, t: b.t, dead: b.dead };
    }
    return out;
}

function branch_length(b)
{
    let l = 0;
    for (let i = 1; i < b.pts.length; i++) l += vlen(vsub(b.pts[i], b.pts[i - 1]));
    return l;
}

// Position, direction and radius at arc fraction t.
function branch_at(b, t)
{
    const total = branch_length(b), target = clamp(t, 0, 1) * total;
    let acc = 0;
    for (let i = 1; i < b.pts.length; i++) {
        const d = vsub(b.pts[i], b.pts[i - 1]), l = vlen(d);
        if (acc + l >= target || i === b.pts.length - 1) {
            const f = l > 0 ? clamp((target - acc) / l, 0, 1) : 0;
            return { p: vmad(b.pts[i - 1], d, f), d: vnorm(d), r: lerp(b.rad[i - 1], b.rad[i], f) };
        }
        acc += l;
    }
    return { p: b.pts[0].slice(), d: [0, 1, 0], r: b.rad[0] };
}

function max_order(sk)
{
    let m = 0;
    for (const b of sk.branches) if (!b.dead) m = Math.max(m, b.order);
    return m;
}

// Moves a branch rigidly so that it starts at its attachment point on the (possibly modified) parent.
function branch_reattach(sk, b)
{
    if (b.parent < 0) return;
    const a = branch_at(sk.branches[b.parent], b.t);
    const off = vsub(a.p, b.pts[0]);
    for (let i = 0; i < b.pts.length; i++) b.pts[i] = vadd(b.pts[i], off);
}

// Polyline from p along dir: per segment the direction turns by curvature (towards up) and droop (towards down,
// growing along the branch) plus random wobble.
function grow_polyline(p, dir, length, segs, curvature, droop, wobble, r)
{
    const pts = [p.slice()];
    let d = vnorm(dir);
    const step = length / segs;
    for (let i = 0; i < segs; i++) {
        const f = (i + 1) / segs;
        d = vnorm(vadd(d, [rng_s(r) * wobble, curvature / segs - droop * f * 2 / segs + rng_s(r) * wobble, rng_s(r) * wobble]));
        p = vmad(p, d, step);
        pts.push(p);
    }
    return pts;
}

// ---------------------------------------------------------------------------------------------------------------
// Operators on skeletons

function make_trunk(P, seed)
{
    const r = rng_make(seed);
    const segs = Math.max(2, Math.round(P.segments));
    const az = rng_f(r) * Math.PI * 2;
    const lean = P.lean * Math.PI / 180;
    const dir = [Math.sin(lean) * Math.cos(az), Math.cos(lean), Math.sin(lean) * Math.sin(az)];
    const pts = grow_polyline([0, 0, 0], dir, P.height, segs, 0, 0, P.wobble, r);
    const rad = [];
    for (let i = 0; i <= segs; i++) {
        const f = i / segs;
        const flare = 1 + P.flare * Math.pow(Math.max(0, 1 - f * 12), 2);    // root flare over the lowest ~8 %
        rad.push(Math.max(0.004, P.radius * Math.pow(1 - f, P.taper) * flare + 0.01 * (1 - f)));
    }
    rad[segs] = Math.max(rad[segs], 0.01);
    let height = 0;
    for (const q of pts) height = Math.max(height, q[1]);
    return { branches: [{ pts: pts, rad: rad, order: 0, parent: -1, t: 0, dead: false }], height: height };
}

const PHYLLO_SPIRAL = 0, PHYLLO_DISTICHOUS = 1, PHYLLO_WHORL = 2;

function add_branches(sk_in, P, seed)
{
    const sk = skel_clone(sk_in);
    const parent_order = max_order(sk);
    const parents = [];
    let longest = 0;
    for (let i = 0; i < sk.branches.length; i++) {
        const b = sk.branches[i];
        if (b.dead || b.order !== parent_order) continue;
        const l = branch_length(b);
        parents.push({ i: i, l: l });
        longest = Math.max(longest, l);
    }
    const segs = Math.max(1, Math.round(P.segments));
    for (const par of parents) {
        const pb = sk.branches[par.i];
        const r = rng_sub(seed, par.i);
        // Longer parents carry more children.
        const n = Math.max(0, Math.round(P.count * Math.sqrt(par.l / Math.max(longest, 1e-6)) + rng_s(r) * 0.5));
        const whorl = Math.max(1, Math.round(P.whorl));
        const az0 = rng_f(r) * Math.PI * 2;
        for (let k = 0; k < n; k++) {
            const rk = rng_sub(hash2(seed, par.i), k);
            let t, az;
            if (P.phyllotaxis === PHYLLO_WHORL) {
                const levels = Math.max(1, Math.ceil(n / whorl)), lev = Math.floor(k / whorl);
                t = lerp(P.start, P.end, (lev + 0.5) / levels);
                az = az0 + (k % whorl) * Math.PI * 2 / whorl + lev * Math.PI / whorl;
            } else {
                t = lerp(P.start, P.end, (k + 0.5 + rng_s(rk) * 0.4) / n);
                az = P.phyllotaxis === PHYLLO_DISTICHOUS ? az0 + (k % 2) * Math.PI : az0 + k * 2.39996;
            }
            az += rng_s(rk) * P.az_jitter * Math.PI / 180;
            t = clamp(t, 0, 1);
            const at = branch_at(pb, t);
            // Side direction perpendicular to the parent, rotated by azimuth; child leaves the parent at 'angle'.
            const side = vrot(vperp(at.d), at.d, az);
            const ang = (P.angle + rng_s(rk) * P.angle_jitter) * Math.PI / 180;
            const dir = vnorm(vadd(vscale(at.d, Math.cos(ang)), vscale(side, Math.sin(ang))));
            const tf = (t - P.start) / Math.max(P.end - P.start, 1e-6);
            const len = par.l * P.length * lerp(1, P.length_top, clamp(tf, 0, 1)) * (1 + rng_s(rk) * 0.15);
            if (len < 0.02) continue;
            const pts = grow_polyline(at.p, dir, len, segs, P.curvature * len, P.droop * len, P.wobble, rk);
            const r0 = Math.max(0.0025, at.r * P.radius);
            const rad = [];
            for (let i = 0; i <= segs; i++) rad.push(Math.max(0.002, r0 * (1 - 0.75 * i / segs)));
            sk.branches.push({ pts: pts, rad: rad, order: parent_order + 1, parent: par.i, t: t, dead: false });
        }
    }
    return sk;
}

// Bends every non-trunk branch towards down (gravity) or up (light), stronger towards the tip and for higher orders.
function apply_tropism(sk_in, P)
{
    const sk = skel_clone(sk_in);
    for (const b of sk.branches) {
        if (b.parent >= 0 && sk.branches[b.parent].dead) b.dead = true;
        if (b.dead) continue;
        branch_reattach(sk, b);
        if (b.order === 0) continue;
        const w = (P.photo - P.gravity) * Math.pow(b.order, P.order_scale);
        const n = b.pts.length - 1;
        const out = [b.pts[0]];
        for (let i = 1; i <= n; i++) {
            const d = vsub(b.pts[i], b.pts[i - 1]), l = vlen(d);
            const nd = vnorm(vadd(vnorm(d), [0, w * i / n, 0]));
            out.push(vmad(out[i - 1], nd, l));
        }
        b.pts = out;
    }
    return sk;
}

const ENV_ELLIPSOID = 0, ENV_EGG = 1, ENV_CONE = 2, ENV_CYLINDER = 3;

function envelope_radius(shape, u)
{
    if (u < 0 || u > 1) return -1;
    switch (shape) {
    case ENV_ELLIPSOID: return Math.sqrt(Math.max(0, 1 - (2 * u - 1) * (2 * u - 1)));
    case ENV_EGG: return 2.13 * Math.sqrt(u) * Math.pow(1 - u, 0.6);
    case ENV_CONE: return 1 - u;
    default: return 1;
    }
}

// Cuts (or shrinks) branches to a crown envelope around the trunk; branches starting below the crown base are removed.
function apply_prune(sk_in, P)
{
    const sk = skel_clone(sk_in);
    const trunk = sk.branches[0];
    const H = sk.height, base = P.crown_base * H, top = H * 1.03, W = P.width * H;
    const t0 = trunk.pts[0], t1 = trunk.pts[trunk.pts.length - 1];
    const inside = function (p) {
        const u = (p[1] - base) / (top - base);
        const rr = envelope_radius(P.shape, u);
        if (rr < 0) return false;
        const c = vlerp(t0, t1, clamp(p[1] / H, 0, 1));
        const dx = p[0] - c[0], dz = p[2] - c[2];
        return dx * dx + dz * dz <= W * W * rr * rr;
    };
    const scale = new Float32Array(sk.branches.length).fill(1);
    for (let bi = 0; bi < sk.branches.length; bi++) {
        const b = sk.branches[bi];
        if (b.parent >= 0 && sk.branches[b.parent].dead) b.dead = true;
        if (b.dead || b.order === 0) continue;
        branch_reattach(sk, b);
        if (b.order < P.min_order) continue;
        // Inherit the parent's shrink so children keep their proportion.
        const s_in = b.parent >= 0 ? scale[b.parent] : 1;
        if (s_in < 1) for (let i = 1; i < b.pts.length; i++) b.pts[i] = vadd(b.pts[0], vscale(vsub(b.pts[i], b.pts[0]), s_in));
        if (!inside(b.pts[0])) { b.dead = true; continue; }
        if (P.mode === 0) {         // cut at the first point outside
            let k = 1;
            while (k < b.pts.length && inside(b.pts[k])) k++;
            if (k === b.pts.length) { scale[bi] = s_in; continue; }
            const old_len = branch_length(b);
            // Boundary point by bisection on the outgoing segment.
            let a = 0, c = 1;
            for (let it = 0; it < 10; it++) { const m = (a + c) / 2; if (inside(vlerp(b.pts[k - 1], b.pts[k], m))) a = m; else c = m; }
            b.pts = b.pts.slice(0, k).concat([vlerp(b.pts[k - 1], b.pts[k], a)]);
            b.rad = b.rad.slice(0, k + 1);
            const f = branch_length(b) / Math.max(old_len, 1e-6);
            if (f < P.min_keep) { b.dead = true; continue; }
            // Children attached beyond the cut die; the others keep their absolute position.
            for (let ci = bi + 1; ci < sk.branches.length; ci++) {
                const c2 = sk.branches[ci];
                if (c2.parent !== bi) continue;
                c2.t = c2.t / f;
                if (c2.t > 1) c2.dead = true;
            }
            scale[bi] = s_in;
        } else {                    // shrink about the base until the tip fits
            let lo = 0, hi = 1;
            const orig = b.pts.map(p => p.slice());
            const fits = function (s) {
                for (let i = 1; i < orig.length; i++) if (!inside(vadd(orig[0], vscale(vsub(orig[i], orig[0]), s)))) return false;
                return true;
            };
            if (fits(1)) { scale[bi] = s_in; continue; }
            for (let it = 0; it < 12; it++) { const m = (lo + hi) / 2; if (fits(m)) lo = m; else hi = m; }
            if (lo < P.min_keep) { b.dead = true; continue; }
            for (let i = 1; i < b.pts.length; i++) b.pts[i] = vadd(orig[0], vscale(vsub(orig[i], orig[0]), lo));
            scale[bi] = s_in * lo;
        }
    }
    return sk;
}

// ---------------------------------------------------------------------------------------------------------------
// Meshes. Mesh = { parts: [Part] }; Part = { name, mat ('bark' | 'leaves'), pos, nrm, uv, col (flat number arrays),
// idx (triangle indices) }.

function part_make(name, mat) { return { name: name, mat: mat, pos: [], nrm: [], uv: [], col: [], idx: [] }; }

function part_vertex(pt, p, n, u, v, c)
{
    pt.pos.push(p[0], p[1], p[2]);
    pt.nrm.push(n[0], n[1], n[2]);
    pt.uv.push(u, v);
    pt.col.push(c[0], c[1], c[2]);
    return pt.pos.length / 3 - 1;
}

// Tubes along every living branch: parallel-transport frames, cylindrical uv with constant texel density
// (u wraps an integer number of bark tiles around the circumference, v = arc length / tile size).
function mesh_wood(sk, P)
{
    const pt = part_make('wood', 'bark');
    const R0 = sk.branches[0].rad[0];
    const white = [1, 1, 1];
    for (const b of sk.branches) {
        if (b.dead || b.rad[0] < P.min_radius) continue;
        const sides = clamp(Math.round(P.sides * Math.sqrt(b.rad[0] / R0)), 3, P.sides);
        const reps = Math.max(1, Math.round(2 * Math.PI * b.rad[0] / P.tile));
        const pts = b.pts.slice();
        // Sink the base into the parent so the joint has no visible gap.
        if (b.parent >= 0) {
            const d0 = vnorm(vsub(pts[1], pts[0]));
            const pr = branch_at(sk.branches[b.parent], b.t).r;
            pts[0] = vmad(pts[0], d0, -pr * 0.7);
        }
        let nrm = null, arc = 0;
        const base = pt.pos.length / 3;
        for (let i = 0; i < pts.length; i++) {
            const d = vnorm(i + 1 < pts.length ? vsub(pts[i + 1], pts[i]) : vsub(pts[i], pts[i - 1]));
            nrm = nrm === null ? vperp(d) : vnorm(vsub(nrm, vscale(d, vdot(nrm, d))));
            const bin = vcross(d, nrm);
            if (i > 0) arc += vlen(vsub(pts[i], pts[i - 1]));
            const r = b.rad[i];
            for (let k = 0; k <= sides; k++) {
                const a = k / sides * Math.PI * 2;
                const n = vadd(vscale(nrm, Math.cos(a)), vscale(bin, Math.sin(a)));
                part_vertex(pt, vmad(pts[i], n, r), n, k / sides * reps, arc / P.tile, white);
            }
        }
        const ring = sides + 1;
        for (let i = 0; i + 1 < pts.length; i++)
            for (let k = 0; k < sides; k++) {
                const a = base + i * ring + k, c = a + ring;
                pt.idx.push(a, a + 1, c + 1, a, c + 1, c);
            }
        // Tip cap: a fan to a point slightly beyond the last ring.
        const last = pts.length - 1, dl = vnorm(vsub(pts[last], pts[last - 1]));
        const tip = part_vertex(pt, vmad(pts[last], dl, b.rad[last]), dl, 0.5 * reps, (arc + b.rad[last]) / P.tile, white);
        for (let k = 0; k < sides; k++) pt.idx.push(base + last * ring + k, base + last * ring + k + 1, tip);
    }
    return { parts: [pt] };
}

// Leaf organ template in leaf space: x along the midrib (petiole from 0 to x0, blade from x0 to x0 + bl), y across,
// z up (fold and curl). uv covers the blade's bounding box: u = (x - x0) / bl, v = 0.5 + y / w.
const LEAF_OVATE = 0, LEAF_ELLIPTIC = 1, LEAF_LOBED = 2, LEAF_PALMATE = 3, LEAF_NEEDLE = 4;

function leaf_half_width(P, s)
{
    const W = P.width / 2;
    let w;
    switch (P.outline) {
    case LEAF_OVATE: w = W * Math.pow(Math.sin(Math.PI * Math.pow(s, 0.62)), 0.9); break;
    case LEAF_ELLIPTIC: w = W * Math.pow(Math.sin(Math.PI * s), 0.8); break;
    case LEAF_LOBED: {
        const env = W * Math.pow(Math.sin(Math.PI * Math.pow(s, 0.8)), 0.8);
        w = env * (1 - P.tooth_depth * 0.5 * (1 - Math.cos(2 * Math.PI * Math.max(1, P.teeth) * s)));
        break;
    }
    case LEAF_NEEDLE: w = W * Math.pow(Math.sin(Math.PI * s), 0.25); break;
    default: w = W * Math.sin(Math.PI * s);
    }
    if (P.outline !== LEAF_LOBED && P.teeth > 0) w *= 1 - P.tooth_depth * ((s * P.teeth) % 1);
    return Math.max(0, w);
}

function make_leaf_shape(P, seed)
{
    const r = rng_make(seed);
    const segs = Math.max(2, Math.round(P.detail));
    const bl = P.length, x0 = P.petiole, w = P.width;
    const asym = 1 + rng_s(r) * 0.08;
    const verts = [], uv = [], tris = [];
    const lift = function (x, y) { const s = (x - x0) / bl; return P.fold * Math.abs(y) - P.curl * s * s * bl; };
    const add = function (x, y) { verts.push([x, y, lift(x, y)]); uv.push([(x - x0) / bl, 0.5 + y / w]); return verts.length - 1; };
    const outline = [];     // closed polygon in (u, v), for the texture mask
    if (P.outline === LEAF_PALMATE) {
        const R = bl / 2, cx = x0 + R, n = segs * 6;
        const c = add(cx, 0);
        const ring = [];
        for (let i = 0; i <= n; i++) {
            const th = -Math.PI + i / n * Math.PI * 2;        // th = 0 points to the tip, +-pi to the petiole
            const lobes = Math.pow(Math.abs(Math.cos(th * 2.5)), 0.7);
            const rr = R * (0.45 + 0.55 * lobes) * (1 - 0.35 * Math.pow(Math.abs(th) / Math.PI, 4)) * (1 - P.tooth_depth * 0.3 * ((i * 0.7) % 1));
            const x = cx + Math.cos(th) * rr, y = Math.sin(th) * rr * (w / bl);
            ring.push(add(x, y));
            outline.push([(x - x0) / bl, 0.5 + y / w]);
        }
        for (let i = 0; i < n; i++) tris.push(c, ring[i], ring[i + 1]);
    } else {
        const mid = [], right = [], left = [];
        for (let i = 0; i <= segs; i++) {
            const s = i / segs, x = x0 + s * bl;
            const hw = leaf_half_width(P, s);
            mid.push(add(x, 0));
            right.push(i === 0 || i === segs ? -1 : add(x, hw * asym));
            left.push(i === 0 || i === segs ? -1 : add(x, -hw / asym));
        }
        for (let i = 0; i < segs; i++) {
            for (const side of [right, left]) {
                const flip = side === left;
                const a = mid[i], b = mid[i + 1], ea = side[i], eb = side[i + 1];
                const tri = function (p, q, s2) { if (flip) tris.push(p, s2, q); else tris.push(p, q, s2); };
                if (ea < 0 && eb < 0) continue;
                if (ea < 0) tri(a, b, eb);
                else if (eb < 0) tri(a, b, ea);
                else { tri(a, eb, ea); tri(a, b, eb); }
            }
        }
        const n = segs * 8;
        for (let i = 0; i <= n; i++) { const s = i / n; outline.push([s, 0.5 + leaf_half_width(P, s) * asym / w]); }
        for (let i = n; i >= 0; i--) { const s = i / n; outline.push([s, 0.5 - leaf_half_width(P, s) / asym / w]); }
    }
    // Vertex normals of the folded template (z up when flat).
    const nrm = verts.map(() => [0, 0, 0]);
    for (let i = 0; i < tris.length; i += 3) {
        const a = verts[tris[i]], b = verts[tris[i + 1]], c = verts[tris[i + 2]];
        const fn = vcross(vsub(b, a), vsub(c, a));
        for (let k = 0; k < 3; k++) nrm[tris[i + k]] = vadd(nrm[tris[i + k]], fn);
    }
    for (let i = 0; i < nrm.length; i++) { nrm[i] = vnorm(nrm[i]); if (nrm[i][2] < 0) nrm[i] = vscale(nrm[i], -1); }
    return { verts: verts, nrm: nrm, uv: uv, tris: tris, outline: outline, kind: P.outline, length: x0 + bl, blade: bl, width: w,
             veins: Math.max(2, Math.round(P.veins)) };
}

// Leaf placements on the 'orders' highest branch orders, on the outer part (from t0) of each branch, plus one at the tip.
function place_leaves(sk, organ, P, seed)
{
    const out = [];
    const up = [0, 1, 0];
    const min_order = max_order(sk) - Math.max(1, P.orders) + 1;
    for (let bi = 0; bi < sk.branches.length; bi++) {
        const b = sk.branches[bi];
        if (b.dead || b.order < min_order) continue;
        const r = rng_sub(seed, bi);
        const len = branch_length(b);
        const n = Math.floor(P.density * len * (1 - P.start) + rng_f(r));
        const az0 = rng_f(r) * Math.PI * 2;
        for (let j = 0; j <= n; j++) {
            const rj = rng_sub(hash2(seed, bi), j);
            const tip = j === n;
            const t = tip ? 1 : P.start + (1 - P.start) * clamp((j + 0.5 + rng_s(rj) * 0.4) / Math.max(n, 1), 0, 1);
            const at = branch_at(b, t);
            let axis;
            if (tip) axis = at.d;
            else {
                const az = P.arrangement === PHYLLO_DISTICHOUS ? az0 + (j % 2) * Math.PI + rng_s(rj) * 0.3 : az0 + j * 2.39996;
                const lateral = vrot(vperp(at.d), at.d, az);
                const ang = (P.angle + rng_s(rj) * 15) * Math.PI / 180;
                axis = vnorm(vadd(vscale(at.d, Math.cos(ang)), vscale(lateral, Math.sin(ang))));
            }
            axis = vnorm(vadd(axis, [0, -P.droop, 0]));
            let side = vcross(up, axis);
            side = vlen(side) < 1e-4 ? vperp(axis) : vnorm(side);
            const roll = rng_s(rj) * P.roll * Math.PI / 180;
            side = vrot(side, axis, roll);
            const nrm = vcross(axis, side);
            const s = P.scale * (1 + rng_s(rj) * P.size_jitter);
            out.push({ p: at.p, axis: axis, side: side, nrm: nrm, s: s, tint: rng_next(rj) });
        }
    }
    return out;
}

function mesh_leaves(placements, organ, P)
{
    const pt = part_make('part:leaves', 'leaves');
    for (const L of placements) {
        const r = rng_make(L.tint);
        const br = 1 + rng_s(r) * P.color_var, hue = rng_s(r) * P.hue_var;
        const col = [clamp(br * (1 + hue), 0, 2), clamp(br, 0, 2), clamp(br * (1 - hue), 0, 2)];
        const base = pt.pos.length / 3;
        const k = L.s;                   // the template is in metres; s is a per-leaf scale factor
        for (let i = 0; i < organ.verts.length; i++) {
            const v = organ.verts[i], n = organ.nrm[i];
            const p = vadd(L.p, vadd(vscale(L.axis, v[0] * k), vadd(vscale(L.side, v[1] * k), vscale(L.nrm, v[2] * k))));
            const nn = vnorm(vadd(vscale(L.axis, n[0]), vadd(vscale(L.side, n[1]), vscale(L.nrm, n[2]))));
            part_vertex(pt, p, nn, organ.uv[i][0], organ.uv[i][1], col);
        }
        for (const t of organ.tris) pt.idx.push(base + t);
    }
    return { parts: [pt] };
}

function mesh_merge(meshes)
{
    const parts = [];
    for (const m of meshes) if (m) for (const p of m.parts) parts.push(p);
    return { parts: parts };
}

function mesh_triangles(m)
{
    let n = 0;
    for (const p of m.parts) n += p.idx.length / 3;
    return n;
}
