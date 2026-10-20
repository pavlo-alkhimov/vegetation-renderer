// Procedural textures: periodic value noise, fBm, periodic Voronoi, height -> normal, bark and leaf generators.
// Texture = { size, albedo, normal, mr } with RGBA8 pixel arrays (albedo sRGB; normal tangent space, +z out;
// mr = glTF metallicRoughness: g = roughness, b = metalness 0). Bark tiles in both directions.

function tex_alloc(size)
{
    const n = size * size * 4;
    return { size: size, albedo: new Uint8ClampedArray(n), normal: new Uint8ClampedArray(n), mr: new Uint8ClampedArray(n) };
}

function hex_rgb(h)
{
    const v = parseInt(h.slice(1), 16);
    return [(v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255];
}

function wrap_i(i, p) { return ((i % p) + p) % p; }

// Periodic value noise from a precomputed lattice: px x py random values, bilinear with smoothstep weights.
function noise_grid(px, py, seed)
{
    const g = new Float32Array(px * py);
    for (let y = 0; y < py; y++) for (let x = 0; x < px; x++) g[y * px + x] = hash2(hash2(seed, x), y) / 4294967296;
    return { px: px, py: py, g: g };
}

function grid_sample(n, x, y)
{
    const ix = Math.floor(x), iy = Math.floor(y);
    let fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    const px = n.px, py = n.py, g = n.g;
    const x0 = ((ix % px) + px) % px, y0 = ((iy % py) + py) % py, x1 = x0 + 1 === px ? 0 : x0 + 1, y1 = y0 + 1 === py ? 0 : y0 + 1;
    const a = g[y0 * px + x0], b = g[y0 * px + x1], c = g[y1 * px + x0], d = g[y1 * px + x1];
    const top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}

// Periodic fBm over the unit tile (base lattice px x py, octaves double the frequency); returns f(u, v) in ~[0, 1].
function make_fbm(px, py, octaves, falloff, seed)
{
    const grids = [], amps = [];
    let amp = 1, norm = 0;
    for (let o = 0; o < octaves; o++) {
        grids.push(noise_grid(px << o, py << o, hash2(seed, o)));
        amps.push(amp); norm += amp; amp *= falloff;
    }
    for (let o = 0; o < octaves; o++) amps[o] /= norm;
    return function (u, v) {
        let s = 0;
        for (let o = 0; o < grids.length; o++) s += amps[o] * grid_sample(grids[o], u * grids[o].px, v * grids[o].py);
        return s;
    };
}

// Periodic Voronoi over the unit tile with cx x cy jittered cells. Returns f(u, v, out) filling f1, f2 (cell units)
// and the nearest cell's hash.
function make_voronoi(cx, cy, seed)
{
    const n = cx * cy, fx = new Float32Array(n), fy = new Float32Array(n), id = new Uint32Array(n);
    for (let y = 0; y < cy; y++)
        for (let x = 0; x < cx; x++) {
            const h = hash2(hash2(seed, x), y), i = y * cx + x;
            fx[i] = (h & 0xffff) / 65536; fy[i] = (h >>> 16) / 65536; id[i] = hash_u32(h);
        }
    return function (u, v, out) {
        const x = u * cx, y = v * cy, ix = Math.floor(x), iy = Math.floor(y);
        let f1 = 1e9, f2 = 1e9, best = 0;
        for (let j = -1; j <= 1; j++) {
            const gy = iy + j, wy = ((gy % cy) + cy) % cy;
            for (let i = -1; i <= 1; i++) {
                const gx = ix + i, wx = ((gx % cx) + cx) % cx, k = wy * cx + wx;
                const dx = gx + fx[k] - x, dy = gy + fy[k] - y, d = dx * dx + dy * dy;
                if (d < f1) { f2 = f1; f1 = d; best = id[k]; } else if (d < f2) f2 = d;
            }
        }
        out.f1 = Math.sqrt(f1); out.f2 = Math.sqrt(f2); out.id = best;
        return out;
    };
}
// Tangent-space normals from a wrapped height field (heights in texels of 'strength' scale).
function tex_normals(t, h, strength)
{
    const n = t.size;
    for (let y = 0; y < n; y++)
        for (let x = 0; x < n; x++) {
            const hl = h[y * n + wrap_i(x - 1, n)], hr = h[y * n + wrap_i(x + 1, n)];
            const hd = h[wrap_i(y - 1, n) * n + x], hu = h[wrap_i(y + 1, n) * n + x];
            const nx = (hl - hr) * strength, ny = (hd - hu) * strength;
            const l = 1 / Math.sqrt(nx * nx + ny * ny + 1);
            const o = (y * n + x) * 4;
            t.normal[o] = (nx * l * 0.5 + 0.5) * 255;
            t.normal[o + 1] = (ny * l * 0.5 + 0.5) * 255;
            t.normal[o + 2] = (l * 0.5 + 0.5) * 255;
            t.normal[o + 3] = 255;
        }
}

function put_rgb(arr, o, c)
{
    arr[o] = Math.pow(clamp(c[0], 0, 1), 1) * 255;
    arr[o + 1] = clamp(c[1], 0, 1) * 255;
    arr[o + 2] = clamp(c[2], 0, 1) * 255;
    arr[o + 3] = 255;
}

function make_bark_texture(P, seed)
{
    const n = P.size, t = tex_alloc(n), h = new Float32Array(n * n);
    const c0 = hex_rgb(P.color_dark), c1 = hex_rgb(P.color_light), cm = hex_rgb(P.moss_color), cl = hex_rgb(P.lenticel_color);
    const fx = Math.max(1, Math.round(P.furrows)), fy = Math.max(1, Math.round(P.furrows / Math.max(P.stretch, 0.1)));
    const plates = Math.max(1, Math.round(P.plates)), len = Math.max(1, Math.round(P.lenticels));
    const vo = { f1: 0, f2: 0, id: 0 };
    const f_detail = make_fbm(8, 8, P.octaves, 0.55, hash2(seed, 1)), f_furrow = make_fbm(fx, fy, 3, 0.5, hash2(seed, 2));
    const v_plate = make_voronoi(plates, Math.max(1, Math.round(plates / Math.max(P.stretch, 0.1))), hash2(seed, 3));
    const v_lent = make_voronoi(len, len * 3, hash2(seed, 4)), f_moss = make_fbm(3, 3, 4, 0.6, hash2(seed, 5));
    for (let y = 0; y < n; y++) {
        const v = (y + 0.5) / n;
        for (let x = 0; x < n; x++) {
            const u = (x + 0.5) / n, i = y * n + x, o = i * 4;
            const detail = f_detail(u, v);
            // Vertical furrows: zero crossings of stretched noise.
            const f = f_furrow(u, v);
            const crack = 1 - smoothstep(0, P.furrow_width, Math.abs(2 * f - 1));
            // Plates: Voronoi cells with raised centres and dark seams.
            v_plate(u, v, vo);
            const seam = 1 - smoothstep(0, 0.08, vo.f2 - vo.f1);
            const plate_h = (vo.id & 255) / 255;
            // Lenticels: short horizontal dashes (cells stretched across u).
            v_lent(u, v, vo);
            const lent = (1 - smoothstep(0.08, 0.16, vo.f1)) * ((vo.id >>> 8 & 255) < 140 ? 1 : 0);
            let hh = 0.5 + (detail - 0.5) * 0.6 - crack * P.furrow_depth - seam * P.plate_depth * 0.6 + plate_h * P.plate_depth * 0.3;
            hh -= lent * 0.15;
            h[i] = hh;
            let tone = clamp(0.5 + (hh - 0.5) * 1.6 + (detail - 0.5) * 0.4, 0, 1);
            let c = vlerp(c0, c1, tone);
            c = vlerp(c, cl, clamp(lent * P.lenticel_contrast + crack * P.furrow_depth * 0.8, 0, 1));
            const moss = smoothstep(1 - P.moss, 1 - P.moss + 0.15, f_moss(u, v) + crack * 0.2);
            c = vlerp(c, vscale(cm, 0.7 + detail * 0.6), moss);
            put_rgb(t.albedo, o, c);
            t.mr[o] = 255; t.mr[o + 1] = clamp(P.roughness + crack * 0.1, 0, 1) * 255; t.mr[o + 2] = 0; t.mr[o + 3] = 255;
        }
    }
    tex_normals(t, h, P.normal_strength * n / 64);
    t.kind = 'bark';
    return t;
}

function dist_segment(px, py, ax, ay, bx, by)
{
    const dx = bx - ax, dy = by - ay;
    const t = clamp(((px - ax) * dx + (py - ay) * dy) / Math.max(dx * dx + dy * dy, 1e-12), 0, 1);
    const ex = ax + dx * t - px, ey = ay + dy * t - py;
    return Math.sqrt(ex * ex + ey * ey);
}

// Leaf maps over the blade's uv box. Veins: midrib, secondary veins from the midrib towards the margin (radial
// from the centre for palmate leaves), tertiary network from Voronoi seams. Outside the outline the blade colour
// continues (padding for filtering); the outline itself is mesh geometry.
function make_leaf_texture(organ, P, seed)
{
    const n = P.size, t = tex_alloc(n), h = new Float32Array(n * n);
    const cb = hex_rgb(P.color), cv = hex_rgb(P.vein_color);
    const bl = organ.blade, w = organ.width, veins = organ.veins;
    const segs = [];   // vein segments in metric leaf coordinates (x along blade, y across), with width
    if (organ.kind === LEAF_PALMATE) {
        const R = bl / 2;
        for (let k = 0; k < 5; k++) {
            const a = (k - 2) * 72 * Math.PI / 180;
            segs.push([R, 0, R + Math.cos(a) * R * 0.95, Math.sin(a) * R * 0.95 * (w / bl), 1]);
        }
    } else {
        segs.push([0, 0, bl, 0, 1]);
        const a = P.vein_angle * Math.PI / 180;
        for (let i = 1; i <= veins; i++) {
            const x = bl * 0.92 * i / (veins + 1);
            const l = w * 0.55 / Math.sin(a);
            for (const sgn of [1, -1]) segs.push([x, 0, x + Math.cos(a) * l, sgn * Math.sin(a) * l, 0.55]);
        }
    }
    for (const s of segs) s.push(Math.max(1e-6, Math.sqrt((s[2] - s[0]) * (s[2] - s[0]) + (s[3] - s[1]) * (s[3] - s[1]))));
    const mw = P.vein_width * w;   // midrib half width in metres
    const vo = { f1: 0, f2: 0, id: 0 };
    const tert = Math.max(2, Math.round(P.network));
    const v_net = make_voronoi(tert, Math.max(2, Math.round(tert * w / bl)), hash2(seed, 7)), f_var = make_fbm(3, 3, 3, 0.5, hash2(seed, 8));
    // Outline mask per row from polygon crossings (scanline), instead of a point-in-polygon test per texel.
    const mask = new Uint8Array(n * n);
    for (let y = 0; y < n; y++) {
        const v = (y + 0.5) / n, xs = [];
        const poly = organ.outline;
        for (let a = 0, b = poly.length - 1; a < poly.length; b = a++) {
            const ya = poly[a][1], yb = poly[b][1];
            if ((ya > v) !== (yb > v)) xs.push(poly[a][0] + (v - ya) / (yb - ya) * (poly[b][0] - poly[a][0]));
        }
        xs.sort((p, q) => p - q);
        for (let k = 0; k + 1 < xs.length; k += 2)
            for (let x = Math.max(0, Math.ceil(xs[k] * n - 0.5)); x <= Math.min(n - 1, Math.floor(xs[k + 1] * n - 0.5)); x++) mask[y * n + x] = 1;
    }
    for (let y = 0; y < n; y++) {
        const v = (y + 0.5) / n, my = (v - 0.5) * w;
        for (let x = 0; x < n; x++) {
            const u = (x + 0.5) / n, mx = u * bl, i = y * n + x, o = i * 4;
            let vein = 0;
            for (const s of segs) {
                const d = dist_segment(mx, my, s[0], s[1], s[2], s[3]);
                const ax = mx - s[0], ay = my - s[1];
                const taper = s[4] * (1 - 0.6 * clamp(Math.sqrt(ax * ax + ay * ay) / s[5], 0, 1));
                vein = Math.max(vein, 1 - smoothstep(mw * taper * 0.5, mw * taper, d));
            }
            v_net(u, v, vo);
            const net = (1 - smoothstep(0, 0.07, vo.f2 - vo.f1)) * 0.35;
            const var_n = f_var(u, v);
            let c = vscale(cb, 1 + (var_n - 0.5) * P.color_var * 2);
            c = vlerp(c, cv, clamp(vein * P.vein_contrast + net * P.vein_contrast * 0.5, 0, 1));
            const edge = mask[i];
            if (!edge) c = vscale(cb, 0.9);
            put_rgb(t.albedo, o, c);
            h[i] = -(vein + net * 0.5) * 0.5 * edge + (var_n - 0.5) * 0.1;
            t.mr[o] = 255; t.mr[o + 1] = clamp(P.roughness, 0, 1) * 255; t.mr[o + 2] = 0; t.mr[o + 3] = 255;
        }
    }
    tex_normals(t, h, P.normal_strength * n / 64);
    t.kind = 'leaf';
    return t;
}

// RGBA8 array -> canvas (for preview, thumbnails and PNG export).
function rgba_canvas(rgba, size)
{
    const cv = document.createElement('canvas');
    cv.width = cv.height = size;
    cv.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(rgba), size, size), 0, 0);
    return cv;
}
