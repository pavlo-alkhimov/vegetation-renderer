// WebGL2 renderer: Gaussian splats (EWA projection, sorted back to front, premultiplied alpha), contour rings, centres,
// ground grid, 3D cursor, selection rings.

// Sun direction (towards the sun), world space, y up.
const SUN = vnorm([0.45, 0.8, 0.35]);

// Opacity factor of selected splats in the splat view (their sections are drawn on top).
const SEL_FADE = 0.2;

const VS_SPLAT = `#version 300 es
#define SEL_FADE ${SEL_FADE.toFixed(3)}
layout(location=0) in vec2 a_corner;
layout(location=1) in vec3 i_pos; layout(location=2) in vec3 i_scale; layout(location=3) in vec4 i_rot; layout(location=4) in vec4 i_col;
layout(location=5) in float i_sel; layout(location=6) in float i_hard; layout(location=7) in float i_mode;
uniform mat4 u_view, u_proj; uniform vec2 u_vp; uniform float u_focal; uniform int u_lit, u_edge; uniform vec3 u_sun, u_eye;
out vec4 v_col; out vec2 v_uv; flat out vec2 v_edge; flat out int v_mode;    // edge parameter per mode (see FS_SPLAT), hardness
mat3 qmat(vec4 q) {
    float w = q.x, x = q.y, y = q.z, z = q.w;
    return mat3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y),
                2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x),
                2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y));
}
void main() {
    mat3 R = qmat(normalize(i_rot));
    mat3 M = R * mat3(i_scale.x, 0.0, 0.0, 0.0, i_scale.y, 0.0, 0.0, 0.0, i_scale.z);
    vec4 vc = u_view * vec4(i_pos, 1.0);
    if (vc.z > -0.005) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }
    // EWA: 2D covariance (pixels^2) = J W Sigma W^T J^T, J = Jacobian of the perspective projection at the centre.
    // The Jacobian is linear only near the view axis: for a splat beside the camera (centre just in front of the camera
    // plane, far off to the side) the vc.x / tz terms explode and the quad covers the whole screen. As in 3DGS, the
    // direction used for J is clamped to 1.3 x the frustum half-angle; the quad stays at the true centre, off screen.
    float tz = -vc.z;
    vec2 lim = 1.3 * 0.5 * u_vp / u_focal;                            // 1.3 x tan(half fov) in x and y
    vec2 txy = clamp(vc.xy / tz, -lim, lim) * tz;
    mat3 J = mat3(u_focal / tz, 0.0, 0.0, 0.0, u_focal / tz, 0.0, u_focal * txy.x / (tz * tz), u_focal * txy.y / (tz * tz), 0.0);
    mat3 T = J * mat3(u_view) * M;
    mat3 cov = T * transpose(T);
    float a = cov[0][0] + 0.3, b = cov[0][1], c = cov[1][1] + 0.3;     // + 0.3 px^2: no splat thinner than ~a pixel
    float mid = 0.5 * (a + c), r = sqrt(max(0.25 * (a - c) * (a - c) + b * b, 1e-12));
    float l1 = mid + r, l2 = max(mid - r, 0.05);
    vec2 e1 = abs(b) > 1e-9 ? normalize(vec2(b, l1 - a)) : (a >= c ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    vec2 e2 = vec2(-e1.y, e1.x);
    // Edge modes (the splat's own, or u_edge when >= 0): 0 Gaussian; 1 saturated (opacity gain, clamped); 2 generalized Gaussian exp(-2 (r/2)^(2 beta)),
    // which equals the Gaussian for beta = 1 and keeps its value e^-2 at the 2-sigma contour for any beta; 3 cut-off at
    // the 2-sigma contour. The quad is cut where alpha falls under 1/255, so hard splats get smaller quads.
    float h = clamp(i_hard, 0.0, 1.0), op = max(i_col.a, 1e-3), cut = 3.0, par = 0.0;
    int mode = u_edge >= 0 ? u_edge : int(i_mode + 0.5);
    v_mode = mode;
    if (mode == 1) { par = 1.0 + 7.0 * h; cut = sqrt(2.0 * max(log(255.0 * op * par), 0.01)); }
    else if (mode == 2) {
        // beta = 2^(4h): 1..16. The rim is about 3.9 sigma / beta wide; beta is limited by the minor axis in pixels so the
        // rim stays >= ~2 px (anti-aliasing).
        par = clamp(1.5 * sqrt(l2), 1.0, exp2(4.0 * h));
        cut = min(3.0, 2.0 * pow(0.5 * max(log(255.0 * op), 0.01), 0.5 / par));
    } else if (mode == 3) {
        float lo = h * exp(-2.0);                                     // smoothstep lower end (FS), as Gaussian value
        if (lo > exp(-4.5)) cut = min(3.0, sqrt(-2.0 * log(lo)) + 1.5 / sqrt(l2));
    }
    v_edge = vec2(par, h);
    vec2 off = (a_corner.x * sqrt(l1) * e1 + a_corner.y * sqrt(l2) * e2) * cut;
    vec4 clip = u_proj * vc;
    clip.xy += off * 2.0 / u_vp * clip.w;
    gl_Position = clip;
    v_uv = a_corner * cut;
    vec3 col = i_col.rgb;
    if (u_lit == 1) {
        // Material splat lit by the sun: normal = the shortest axis (flat splats). Two-sided: each face is lit on its own,
        // the face towards the sun gets direct light, the other ambient plus some light through the leaf. The visible face
        // is blended across the plane over an angle band set by the splat's thickness ratio (seen edge-on, a thick splat
        // shows both faces), so crossing the plane changes the light smoothly instead of jumping.
        float smin = min(i_scale.x, min(i_scale.y, i_scale.z)), smax = max(i_scale.x, max(i_scale.y, i_scale.z));
        vec3 n = i_scale.x < i_scale.y ? (i_scale.x < i_scale.z ? R[0] : R[2]) : (i_scale.y < i_scale.z ? R[1] : R[2]);
        float flat_ = 1.0 - smin / smax;
        float ns = dot(n, u_sun);
        float face_a = 0.42 + 0.85 * max(ns, 0.0) + 0.25 * max(-ns, 0.0);   // face along +n
        float face_b = 0.42 + 0.85 * max(-ns, 0.0) + 0.25 * max(ns, 0.0);   // face along -n
        float band = max(smin / smax, 0.05);
        float front = smoothstep(-band, band, dot(n, normalize(u_eye - i_pos)));
        float lit = mix(0.95, mix(face_b, face_a, front), flat_);
        col *= lit;
    }
    v_col = vec4(col, i_col.a * (i_sel > 0.5 ? SEL_FADE : 1.0));     // selected: faded, so its grid overlay reads
}`;

const FS_SPLAT = `#version 300 es
precision highp float;
in vec4 v_col; in vec2 v_uv; flat in vec2 v_edge; flat in int v_mode; out vec4 o;
void main() {
    float q = dot(v_uv, v_uv), g = exp(-0.5 * q), alpha;
    if (v_mode == 1) alpha = v_col.a * min(1.0, v_edge.x * g);                       // saturated: flat top, steeper rim
    else if (v_mode == 2) alpha = v_col.a * exp(-2.0 * pow(0.25 * q, v_edge.x));    // generalized Gaussian
    else if (v_mode == 3) {                                                          // cut-off at the 2-sigma contour
        float t = exp(-2.0), w = max((1.0 - v_edge.y) * t, fwidth(g));               // edge width: hardness, >= ~1 px
        alpha = v_col.a * smoothstep(t - w, t + w, g);
    } else alpha = v_col.a * g;
    if (alpha < 1.0 / 255.0) discard;
    o = vec4(v_col.rgb * alpha, alpha);
}`;

const VS_ELL = `#version 300 es
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 i_pos; layout(location=2) in vec3 i_scale; layout(location=3) in vec4 i_rot; layout(location=4) in vec4 i_col;
layout(location=5) in float i_sel;
uniform mat4 u_vp; uniform float u_k; uniform int u_only_sel;
out vec3 v_n; out vec4 v_col;
mat3 qmat(vec4 q) {
    float w = q.x, x = q.y, y = q.z, z = q.w;
    return mat3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y),
                2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x),
                2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y));
}
void main() {
    if (u_only_sel == 1 && i_sel < 0.5) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }
    mat3 R = qmat(normalize(i_rot));
    vec3 s = max(i_scale, vec3(1e-5));
    gl_Position = u_vp * vec4(i_pos + R * (a_pos * s * u_k), 1.0);
    v_n = R * (a_pos / s);
    v_col = vec4(i_col.rgb, i_sel);
}`;

const FS_ELL = `#version 300 es
precision highp float;
in vec3 v_n; in vec4 v_col; uniform vec3 u_sun; uniform int u_mode; out vec4 o;
void main() {
    if (u_mode == 1) { o = vec4(1.0, 0.62, 0.15, 1.0); return; }                 // selection rings
    vec3 n = normalize(v_n);
    float d = 0.45 + 0.6 * max(dot(n, u_sun), 0.0);
    vec3 c = v_col.rgb * d;
    if (v_col.a > 0.5) c = mix(c, vec3(1.0, 0.62, 0.15), 0.35);
    o = vec4(c, 1.0);
}`;

// Contour rings: sections of the 2-sigma ellipsoid by planes parallel to the splat's three local planes. The sections
// are worked out on the CPU (rings_build), one instance per ring: splat data plus (plane, offset). Plane 0 = xy,
// 1 = yz, 2 = zx; offset f along the plane's normal axis in units of 2 sigma (0 = central section).
const SLICES_MAX = 200;           // sections per axis
const RINGS_MAX = 150000;         // per frame, a safety limit (camera inside large splats)
const VS_RING = `#version 300 es
layout(location=0) in vec3 a_pos;                                     // unit circle in xy
layout(location=1) in vec3 i_pos; layout(location=2) in vec3 i_scale; layout(location=3) in vec4 i_rot;
layout(location=4) in vec4 i_col;                                     // rgb, flag: 0 normal, 1 selected, 2 hover
layout(location=5) in vec2 i_ring;                                    // plane, offset
uniform mat4 u_vp; uniform vec3 u_hover; uniform float u_hover_a;
out vec4 v_col;
mat3 qmat(vec4 q) {
    float w = q.x, x = q.y, y = q.z, z = q.w;
    return mat3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y),
                2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x),
                2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y));
}
void main() {
    int r = int(i_ring.x + 0.5); float f = i_ring.y;
    vec2 c = a_pos.xy * sqrt(max(1.0 - f * f, 0.0));
    vec3 loc = r == 0 ? vec3(c.x, c.y, f) : r == 1 ? vec3(f, c.x, c.y) : vec3(c.x, f, c.y);
    gl_Position = u_vp * vec4(i_pos + qmat(normalize(i_rot)) * (loc * i_scale * 2.0), 1.0);
    const vec3 SEL = vec3(1.0, 0.62, 0.15);
    vec3 col = i_col.a > 1.5 ? u_hover : i_col.a > 0.5 ? SEL : min(i_col.rgb * 1.25 + 0.08, vec3(1.0));
    float alpha = i_col.a > 1.5 ? u_hover_a : 1.0;
    v_col = vec4(f == 0.0 ? col : col * 0.6, alpha);
}`;

const VS_LINE = `#version 300 es
layout(location=0) in vec3 a_pos; layout(location=1) in vec4 a_col;
uniform mat4 u_vp; uniform float u_psize; out vec4 v_col;
void main() { v_col = a_col; gl_Position = u_vp * vec4(a_pos, 1.0); gl_PointSize = u_psize; }`;
const FS_LINE = `#version 300 es
precision highp float; in vec4 v_col; out vec4 o; void main() { o = v_col; }`;

function gl_prog(gl, vs, fs)
{
    const mk = function (type, src) {
        const s = gl.createShader(type);
        gl.shaderSource(s, src); gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
        return s;
    };
    const p = gl.createProgram();
    gl.attachShader(p, mk(gl.VERTEX_SHADER, vs)); gl.attachShader(p, mk(gl.FRAGMENT_SHADER, fs));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    const u = {}, n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < n; i++) { const a = gl.getActiveUniform(p, i); u[a.name] = gl.getUniformLocation(p, a.name); }
    return { p: p, u: u };
}

// Unit sphere: triangles (ellipsoid mode) and three great circles as lines (selection rings).
function sphere_mesh()
{
    const tri = [], ring = [], nu = 16, nv = 10;
    const P = (i, j) => { const th = j / nv * Math.PI, ph = i / nu * 2 * Math.PI; return [Math.sin(th) * Math.cos(ph), Math.cos(th), Math.sin(th) * Math.sin(ph)]; };
    for (let j = 0; j < nv; j++)
        for (let i = 0; i < nu; i++) tri.push(...P(i, j), ...P(i + 1, j), ...P(i + 1, j + 1), ...P(i, j), ...P(i + 1, j + 1), ...P(i, j + 1));
    for (let i = 0; i < 48; i++)
        for (const a of [i, i + 1]) ring.push(Math.cos(a / 48 * 2 * Math.PI), Math.sin(a / 48 * 2 * Math.PI), 0);
    return { tri: new Float32Array(tri), ring: new Float32Array(ring) };
}

function render_init(canvas)
{
    const gl = canvas.getContext('webgl2', { antialias: true, premultipliedAlpha: false });
    if (!gl) throw new Error('WebGL2 not available');
    const R = { gl: gl, canvas: canvas, n: 0, dirty: true, sort_ms: 0 };
    R.ps = gl_prog(gl, VS_SPLAT, FS_SPLAT);
    R.pe = gl_prog(gl, VS_ELL, FS_ELL);
    R.pl = gl_prog(gl, VS_LINE, FS_LINE);
    R.pr = gl_prog(gl, VS_RING, FS_LINE);
    R.inst = gl.createBuffer();
    const mesh = sphere_mesh();
    const vao_for = function (verts, comps, inst_sel) {
        const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
        const vb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, vb); gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);
        gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, comps, gl.FLOAT, false, 0, 0);
        gl.bindBuffer(gl.ARRAY_BUFFER, R.inst);
        const st = 17 * 4, sz = [3, 3, 4, 4, 1, 1, 1], of = [0, 3, 6, 10, 14, 15, 16];   // pos, sigma, rot, rgba, sel, hardness, mode
        for (let k = 0; k < 7; k++) {
            gl.enableVertexAttribArray(1 + k); gl.vertexAttribPointer(1 + k, sz[k], gl.FLOAT, false, st, of[k] * 4); gl.vertexAttribDivisor(1 + k, 1);
        }
        gl.bindVertexArray(null);
        return vao;
    };
    R.vao_splat = vao_for(new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), 2, false);
    R.vao_ell = vao_for(mesh.tri, 3, true); R.ell_count = mesh.tri.length / 3;
    // rings: circle (location 0) + ring instances (16 floats: pos, sigma, rot, rgb+sel, plane, offset)
    R.ring_inst = gl.createBuffer(); R.ring_count = mesh.ring.length / 3;
    R.vao_ring = gl.createVertexArray(); gl.bindVertexArray(R.vao_ring);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer()); gl.bufferData(gl.ARRAY_BUFFER, mesh.ring, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, R.ring_inst);
    [[3, 0], [3, 3], [4, 6], [4, 10], [2, 14]].forEach(function (a, k) {
        gl.enableVertexAttribArray(1 + k); gl.vertexAttribPointer(1 + k, a[0], gl.FLOAT, false, 64, a[1] * 4); gl.vertexAttribDivisor(1 + k, 1);
    });
    gl.bindVertexArray(null);
    R.vao_line = gl.createVertexArray(); gl.bindVertexArray(R.vao_line);
    R.line_vb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, R.line_vb);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 28, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 28, 12);
    gl.bindVertexArray(null);
    return R;
}

// Instance data in back-to-front order for the given view (sorting needed for alpha blending).
function render_upload(R, list, view)
{
    const gl = R.gl, n = list.length, t0 = performance.now();
    const depth = new Float32Array(n), idx = new Uint32Array(n);
    for (let i = 0; i < n; i++) {
        const p = list[i].p;
        depth[i] = view[2] * p[0] + view[6] * p[1] + view[10] * p[2] + view[14];     // view-space z (negative in front)
        idx[i] = i;
    }
    idx.sort((a, b) => depth[a] - depth[b]);                                          // farthest (most negative) first
    const f = new Float32Array(n * 17);
    for (let k = 0; k < n; k++) {
        const t = list[idx[k]], o = k * 17;
        f[o] = t.p[0]; f[o + 1] = t.p[1]; f[o + 2] = t.p[2];
        f[o + 3] = t.s[0]; f[o + 4] = t.s[1]; f[o + 5] = t.s[2];
        f[o + 6] = t.q[0]; f[o + 7] = t.q[1]; f[o + 8] = t.q[2]; f[o + 9] = t.q[3];
        f[o + 10] = t.c[0]; f[o + 11] = t.c[1]; f[o + 12] = t.c[2]; f[o + 13] = t.a; f[o + 14] = t.sel ? 1 : 0; f[o + 15] = t.h; f[o + 16] = t.m;
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, R.inst); gl.bufferData(gl.ARRAY_BUFFER, f, gl.DYNAMIC_DRAW);
    R.n = n;
    R.sort_ms = performance.now() - t0;
}

function lines_draw(R, data, mode, vp, psize)
{
    const gl = R.gl;
    if (!data.length) return;
    gl.useProgram(R.pl.p);
    gl.uniformMatrix4fv(R.pl.u.u_vp, false, vp); gl.uniform1f(R.pl.u.u_psize, psize || 1);
    gl.bindVertexArray(R.vao_line);
    gl.bindBuffer(gl.ARRAY_BUFFER, R.line_vb); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.DYNAMIC_DRAW);
    gl.drawArrays(mode, 0, data.length / 7);
}

// Ground grid: 10 cm lines to 1 m, 1 m lines to 5 m, coloured axes.
function grid_lines()
{
    const d = [], L = (a, b, c) => d.push(...a, ...c, ...b, ...c);
    for (let i = -10; i <= 10; i++) {
        const v = i / 10, c = i === 0 ? null : [0.32, 0.33, 0.36, 1];
        if (c) { L([v, 0, -1], [v, 0, 1], c); L([-1, 0, v], [1, 0, v], c); }
    }
    for (let i = -5; i <= 5; i++) {
        if (i === 0) continue;
        L([i, 0, -5], [i, 0, 5], [0.26, 0.27, 0.3, 1]); L([-5, 0, i], [5, 0, i], [0.26, 0.27, 0.3, 1]);
    }
    L([-5, 0, 0], [5, 0, 0], [0.75, 0.3, 0.3, 1]); L([0, 0, -5], [0, 0, 5], [0.3, 0.45, 0.8, 1]);
    return d;
}

// Ring instances for the splats (only_sel: selected ones). Per splat and local plane: skipped when either in-plane axis
// is under minpx pixels across (2-sigma diameter); otherwise the central section, then pairs at m * space pixels from
// the centre along the normal axis (nominal size, m = 1, 2, ...) while inside the 2-sigma edge. The spacing is fixed on
// screen, not fitted to the splat, so the outermost sections fall wherever the edge is. At most SLICES_MAX per axis.
function rings_build(list, cam, h, minpx, only_sel, space, flag)
{
    const v = cam.view, F = cam.focal * h, out = [];
    let count = 0;
    for (const t of list) {
        if (only_sel && !t.sel) continue;
        const z = -(v[2] * t.p[0] + v[6] * t.p[1] + v[10] * t.p[2] + v[14]), ext = 2 * Math.max(t.s[0], t.s[1], t.s[2]);
        if (z + ext <= 0) continue;                                   // entirely behind the camera
        const depth = Math.max(z, 1e-3), px = t.s.map(s => 4 * s * F / depth);
        for (let r = 0; r < 3; r++) {
            const a = r === 0 ? [0, 1] : r === 1 ? [1, 2] : [0, 2], nk = r === 0 ? 2 : r === 1 ? 0 : 1;
            if (Math.min(px[a[0]], px[a[1]]) < minpx) continue;
            const half = 0.5 * px[nk], sp = Math.max(space, 1);
            const pairs = Math.max(0, Math.min((SLICES_MAX - 1) >> 1, Math.ceil(half / sp) - 1));   // m * sp < half
            for (let m = -pairs; m <= pairs; m++) {
                if (count >= RINGS_MAX) break;
                out.push(t.p[0], t.p[1], t.p[2], t.s[0], t.s[1], t.s[2], t.q[0], t.q[1], t.q[2], t.q[3], t.c[0], t.c[1], t.c[2], flag !== undefined ? flag : t.sel ? 1 : 0, r, m * sp / half);
                count++;
            }
        }
    }
    return new Float32Array(out);
}

// flag / hover_col: draw all rings of the list in hover_col (the splat a click would pick).
function rings_draw(R, cam, vp, h, minpx, only_sel, space, list, flag, hover_col, hover_a)
{
    const gl = R.gl, data = rings_build(list, cam, h, minpx, only_sel, space, flag), n = data.length / 16;
    R.rings = n;
    if (!n) return;
    gl.bindBuffer(gl.ARRAY_BUFFER, R.ring_inst); gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
    gl.useProgram(R.pr.p);
    gl.uniformMatrix4fv(R.pr.u.u_vp, false, vp); gl.uniform3fv(R.pr.u.u_hover, hover_col || [1, 1, 1]); gl.uniform1f(R.pr.u.u_hover_a, hover_a === undefined ? 1 : hover_a);
    gl.bindVertexArray(R.vao_ring);
    gl.drawArraysInstanced(gl.LINES, 0, R.ring_count, n);
}

function render_frame(R, cam, opts)
{
    const gl = R.gl, c = R.canvas, dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.floor(c.clientWidth * dpr)), h = Math.max(1, Math.floor(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    gl.viewport(0, 0, w, h);
    gl.clearColor(opts.bg[0], opts.bg[1], opts.bg[2], 1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const vp = m4_mul(cam.proj, cam.view), sun = SUN;
    gl.enable(gl.DEPTH_TEST); gl.depthMask(true); gl.disable(gl.BLEND);
    if (opts.grid) lines_draw(R, R.grid || (R.grid = grid_lines()), gl.LINES, vp);
    if (R.n) {
        if (opts.mode === 0) {
            gl.depthMask(false); gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
            gl.useProgram(R.ps.p);
            gl.uniformMatrix4fv(R.ps.u.u_view, false, cam.view); gl.uniformMatrix4fv(R.ps.u.u_proj, false, cam.proj);
            gl.uniform2f(R.ps.u.u_vp, w, h); gl.uniform1f(R.ps.u.u_focal, cam.focal * h);
            gl.uniform1i(R.ps.u.u_lit, opts.lit ? 1 : 0); gl.uniform1i(R.ps.u.u_edge, opts.edge === undefined ? -1 : opts.edge); gl.uniform3fv(R.ps.u.u_sun, sun); gl.uniform3fv(R.ps.u.u_eye, cam.eye);
            gl.bindVertexArray(R.vao_splat);
            gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, R.n);
            gl.disable(gl.BLEND); gl.depthMask(true);
        } else if (opts.mode === 1) {
            rings_draw(R, cam, vp, h, 3 * dpr, false, (opts.slice_px || 20) * dpr, opts.list);
        }
        if (opts.mode === 2 || opts.centres) {
            const pts = [];
            for (const t of opts.list) pts.push(...t.p, ...(t.sel ? [1, 0.62, 0.15, 1] : [t.c[0], t.c[1], t.c[2], 1]));
            gl.disable(gl.DEPTH_TEST);
            lines_draw(R, pts, gl.POINTS, vp, opts.mode === 2 ? 5 * dpr : 3 * dpr);
            gl.enable(gl.DEPTH_TEST);
        }
        // Selection: drawn as in the contour view (sections), in orange, always on top.
        if (opts.nsel) {
            gl.disable(gl.DEPTH_TEST);
            rings_draw(R, cam, vp, h, 0, true, (opts.slice_px || 20) * dpr, opts.list);
            gl.enable(gl.DEPTH_TEST);
        }
        // Hover: the splat a click would pick, in a colour telling what the click would do, on top of everything.
        // opts.hover: [{ t: splat, col, a }], the current one (a = 1) and ones fading out after the mouse left them.
        if (opts.hover && opts.hover.length) {
            gl.disable(gl.DEPTH_TEST); gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
            for (const hv of opts.hover) rings_draw(R, cam, vp, h, 0, false, (opts.slice_px || 20) * dpr, [hv.t], 2, hv.col, hv.a);
            gl.disable(gl.BLEND); gl.enable(gl.DEPTH_TEST);
        }
    }
    // 3D cursor.
    const k = cam.dist * 0.03, p = opts.cursor, cc = [1, 0.3, 0.3, 1];
    gl.disable(gl.DEPTH_TEST);
    lines_draw(R, [p[0] - k, p[1], p[2], ...cc, p[0] + k, p[1], p[2], ...cc, p[0], p[1] - k, p[2], ...cc, p[0], p[1] + k, p[2], ...cc,
                   p[0], p[1], p[2] - k, ...cc, p[0], p[1], p[2] + k, ...cc], gl.LINES, vp);
    gl.enable(gl.DEPTH_TEST);
    if (opts.lit) sun_gizmo(R, cam, dpr);
    gl.bindVertexArray(null);
}

// Light gizmo in the lower left corner: world axes (X red, Y green, Z blue), the horizon circle and a yellow line from the
// centre to the sun, all turning with the camera.
function sun_gizmo(R, cam, dpr)
{
    const gl = R.gl, size = Math.round(110 * dpr), pad = Math.round(8 * dpr);
    const rot = Array.from(cam.view); rot[12] = rot[13] = rot[14] = 0;
    const vp = m4_mul([0.8, 0, 0, 0, 0, 0.8, 0, 0, 0, 0, -0.4, 0, 0, 0, 0, 1], rot);
    const L = [], ax = (d, c) => L.push(0, 0, 0, ...c, ...d, ...c);
    ax([0.6, 0, 0], [0.9, 0.3, 0.3, 1]); ax([0, 0.6, 0], [0.4, 0.85, 0.4, 1]); ax([0, 0, 0.6], [0.35, 0.55, 1, 1]);
    for (let i = 0; i < 48; i++) { const a = i / 48 * 6.2832, b = (i + 1) / 48 * 6.2832, g = [0.45, 0.47, 0.5, 1];
        L.push(Math.cos(a), 0, Math.sin(a), ...g, Math.cos(b), 0, Math.sin(b), ...g); }
    const y = [1, 0.85, 0.2, 1], s = SUN;
    L.push(0, 0, 0, ...y, s[0], s[1], s[2], ...y);
    L.push(s[0], 0, s[2], 0.6, 0.5, 0.15, 1, s[0], s[1], s[2], 0.6, 0.5, 0.15, 1);   // drop line to the horizon: elevation
    gl.disable(gl.DEPTH_TEST);
    gl.viewport(pad, pad, size, size);
    lines_draw(R, L, gl.LINES, vp);
    lines_draw(R, [s[0], s[1], s[2], ...y], gl.POINTS, vp, 9 * dpr);
    gl.viewport(0, 0, R.canvas.width, R.canvas.height);
    gl.enable(gl.DEPTH_TEST);
}
