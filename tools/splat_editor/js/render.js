// WebGL2 renderer: Gaussian splats (EWA projection, sorted back to front, premultiplied alpha), ellipsoids, centres,
// ground grid, 3D cursor, selection rings.

const VS_SPLAT = `#version 300 es
layout(location=0) in vec2 a_corner;
layout(location=1) in vec3 i_pos; layout(location=2) in vec3 i_scale; layout(location=3) in vec4 i_rot; layout(location=4) in vec4 i_col;
uniform mat4 u_view, u_proj; uniform vec2 u_vp; uniform float u_focal; uniform int u_lit; uniform vec3 u_sun, u_eye;
out vec4 v_col; out vec2 v_uv;
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
    float tz = -vc.z;
    mat3 J = mat3(u_focal / tz, 0.0, 0.0, 0.0, u_focal / tz, 0.0, u_focal * vc.x / (tz * tz), u_focal * vc.y / (tz * tz), 0.0);
    mat3 T = J * mat3(u_view) * M;
    mat3 cov = T * transpose(T);
    float a = cov[0][0] + 0.3, b = cov[0][1], c = cov[1][1] + 0.3;     // + 0.3 px^2: no splat thinner than ~a pixel
    float mid = 0.5 * (a + c), r = sqrt(max(0.25 * (a - c) * (a - c) + b * b, 1e-12));
    float l1 = mid + r, l2 = max(mid - r, 0.05);
    vec2 e1 = abs(b) > 1e-9 ? normalize(vec2(b, l1 - a)) : (a >= c ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    vec2 e2 = vec2(-e1.y, e1.x);
    vec2 off = (a_corner.x * sqrt(l1) * e1 + a_corner.y * sqrt(l2) * e2) * 3.0;          // quad to 3 sigma
    vec4 clip = u_proj * vc;
    clip.xy += off * 2.0 / u_vp * clip.w;
    gl_Position = clip;
    v_uv = a_corner * 3.0;
    vec3 col = i_col.rgb;
    if (u_lit == 1) {
        // Material splat lit by the sun: normal = the shortest axis (flat splats), facing the camera.
        vec3 n = i_scale.x < i_scale.y ? (i_scale.x < i_scale.z ? R[0] : R[2]) : (i_scale.y < i_scale.z ? R[1] : R[2]);
        float flat_ = 1.0 - min(i_scale.x, min(i_scale.y, i_scale.z)) / max(i_scale.x, max(i_scale.y, i_scale.z));
        if (dot(n, u_eye - i_pos) < 0.0) n = -n;
        float d = max(dot(n, u_sun), 0.0), back = max(-dot(n, u_sun), 0.0);
        float lit = mix(0.95, 0.42 + 0.85 * d + 0.25 * back, flat_);
        col *= lit;
    }
    v_col = vec4(col, i_col.a);
}`;

const FS_SPLAT = `#version 300 es
precision highp float;
in vec4 v_col; in vec2 v_uv; out vec4 o;
void main() {
    float alpha = v_col.a * exp(-0.5 * dot(v_uv, v_uv));
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
    for (let k = 0; k < 3; k++)
        for (let i = 0; i < 32; i++)
            for (const a of [i, i + 1]) {
                const c = Math.cos(a / 32 * 2 * Math.PI), s = Math.sin(a / 32 * 2 * Math.PI);
                ring.push(...(k === 0 ? [c, s, 0] : k === 1 ? [0, c, s] : [c, 0, s]));
            }
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
    R.inst = gl.createBuffer();
    const mesh = sphere_mesh();
    const vao_for = function (verts, comps, inst_sel) {
        const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
        const vb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, vb); gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);
        gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, comps, gl.FLOAT, false, 0, 0);
        gl.bindBuffer(gl.ARRAY_BUFFER, R.inst);
        const st = 15 * 4, sz = [3, 3, 4, 4, 1], of = [0, 3, 6, 10, 14];
        for (let k = 0; k < (inst_sel ? 5 : 4); k++) {
            gl.enableVertexAttribArray(1 + k); gl.vertexAttribPointer(1 + k, sz[k], gl.FLOAT, false, st, of[k] * 4); gl.vertexAttribDivisor(1 + k, 1);
        }
        gl.bindVertexArray(null);
        return vao;
    };
    R.vao_splat = vao_for(new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), 2, false);
    R.vao_ell = vao_for(mesh.tri, 3, true); R.ell_count = mesh.tri.length / 3;
    R.vao_ring = vao_for(mesh.ring, 3, true); R.ring_count = mesh.ring.length / 3;
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
    const f = new Float32Array(n * 15);
    for (let k = 0; k < n; k++) {
        const t = list[idx[k]], o = k * 15;
        f[o] = t.p[0]; f[o + 1] = t.p[1]; f[o + 2] = t.p[2];
        f[o + 3] = t.s[0]; f[o + 4] = t.s[1]; f[o + 5] = t.s[2];
        f[o + 6] = t.q[0]; f[o + 7] = t.q[1]; f[o + 8] = t.q[2]; f[o + 9] = t.q[3];
        f[o + 10] = t.c[0]; f[o + 11] = t.c[1]; f[o + 12] = t.c[2]; f[o + 13] = t.a; f[o + 14] = t.sel ? 1 : 0;
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

// mode: 0 splats, 1 ellipsoids (2 sigma), 2 centres. view: camera {view, proj, eye, focal}.
function render_frame(R, cam, opts)
{
    const gl = R.gl, c = R.canvas, dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.floor(c.clientWidth * dpr)), h = Math.max(1, Math.floor(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    gl.viewport(0, 0, w, h);
    gl.clearColor(opts.bg[0], opts.bg[1], opts.bg[2], 1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const vp = m4_mul(cam.proj, cam.view), sun = vnorm([0.45, 0.8, 0.35]);
    gl.enable(gl.DEPTH_TEST); gl.depthMask(true); gl.disable(gl.BLEND);
    if (opts.grid) lines_draw(R, R.grid || (R.grid = grid_lines()), gl.LINES, vp);
    if (R.n) {
        if (opts.mode === 0) {
            gl.depthMask(false); gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
            gl.useProgram(R.ps.p);
            gl.uniformMatrix4fv(R.ps.u.u_view, false, cam.view); gl.uniformMatrix4fv(R.ps.u.u_proj, false, cam.proj);
            gl.uniform2f(R.ps.u.u_vp, w, h); gl.uniform1f(R.ps.u.u_focal, cam.focal * h);
            gl.uniform1i(R.ps.u.u_lit, opts.lit ? 1 : 0); gl.uniform3fv(R.ps.u.u_sun, sun); gl.uniform3fv(R.ps.u.u_eye, cam.eye);
            gl.bindVertexArray(R.vao_splat);
            gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, R.n);
            gl.disable(gl.BLEND); gl.depthMask(true);
        } else if (opts.mode === 1) {
            gl.useProgram(R.pe.p);
            gl.uniformMatrix4fv(R.pe.u.u_vp, false, vp); gl.uniform1f(R.pe.u.u_k, 2); gl.uniform1i(R.pe.u.u_only_sel, 0);
            gl.uniform3fv(R.pe.u.u_sun, sun); gl.uniform1i(R.pe.u.u_mode, 0);
            gl.bindVertexArray(R.vao_ell);
            gl.drawArraysInstanced(gl.TRIANGLES, 0, R.ell_count, R.n);
        }
        if (opts.mode === 2 || opts.centres) {
            const pts = [];
            for (const t of opts.list) pts.push(...t.p, ...(t.sel ? [1, 0.62, 0.15, 1] : [t.c[0], t.c[1], t.c[2], 1]));
            gl.disable(gl.DEPTH_TEST);
            lines_draw(R, pts, gl.POINTS, vp, opts.mode === 2 ? 5 * dpr : 3 * dpr);
            gl.enable(gl.DEPTH_TEST);
        }
        // Selection: three 2-sigma rings per selected splat, always on top.
        if (opts.nsel) {
            gl.disable(gl.DEPTH_TEST);
            gl.useProgram(R.pe.p);
            gl.uniformMatrix4fv(R.pe.u.u_vp, false, vp); gl.uniform1f(R.pe.u.u_k, 2); gl.uniform1i(R.pe.u.u_only_sel, 1); gl.uniform1i(R.pe.u.u_mode, 1);
            gl.bindVertexArray(R.vao_ring);
            gl.drawArraysInstanced(gl.LINES, 0, R.ring_count, R.n);
            gl.enable(gl.DEPTH_TEST);
        }
    }
    // 3D cursor.
    const k = cam.dist * 0.03, p = opts.cursor, cc = [1, 0.3, 0.3, 1];
    gl.disable(gl.DEPTH_TEST);
    lines_draw(R, [p[0] - k, p[1], p[2], ...cc, p[0] + k, p[1], p[2], ...cc, p[0], p[1] - k, p[2], ...cc, p[0], p[1] + k, p[2], ...cc,
                   p[0], p[1], p[2] - k, ...cc, p[0], p[1], p[2] + k, ...cc], gl.LINES, vp);
    gl.enable(gl.DEPTH_TEST);
    gl.bindVertexArray(null);
}
