// WebGL2 viewport: orbit camera, textured plant meshes (bark / two-sided leaves with transmission), skeleton lines,
// ground disc, 2D texture preview, offscreen thumbnails.

const VS_MESH = `#version 300 es
layout(location=0) in vec3 a_pos; layout(location=1) in vec3 a_nrm; layout(location=2) in vec2 a_uv; layout(location=3) in vec3 a_col;
uniform mat4 u_vp; uniform mat4 u_model; uniform float u_bias;
out vec3 v_pos; out vec3 v_nrm; out vec2 v_uv; out vec3 v_col;
void main() {
    v_pos = (u_model * vec4(a_pos, 1.0)).xyz; v_nrm = mat3(u_model) * a_nrm; v_uv = a_uv; v_col = a_col;
    gl_Position = u_vp * vec4(v_pos, 1.0); gl_Position.z -= u_bias * gl_Position.w;
}`;

const FS_MESH = `#version 300 es
precision highp float;
in vec3 v_pos; in vec3 v_nrm; in vec2 v_uv; in vec3 v_col;
uniform int u_mat;            // 0 bark, 1 leaves, 2 ground, 3 wire (unlit vertex colour)
uniform int u_tex;            // textures bound
uniform int u_mode;           // 0 shaded, 1 albedo, 2 normals
uniform sampler2D u_albedo, u_normal, u_mr;
uniform vec3 u_base, u_sun, u_eye;
out vec4 o;
vec3 to_lin(vec3 c) { return pow(c, vec3(2.2)); }
void main() {
    if (u_mat == 3) { o = vec4(v_col, 1.0); return; }
    vec3 N = normalize(v_nrm);
    bool back = !gl_FrontFacing;
    if (u_mat == 1 && back) N = -N;
    vec3 albedo = to_lin(u_base) * v_col;
    float rough = 0.7;
    if (u_tex == 1 && u_mat != 2) {
        albedo = to_lin(texture(u_albedo, v_uv).rgb) * v_col;
        rough = texture(u_mr, v_uv).g;
        // Cotangent frame from screen derivatives (no tangent attribute needed).
        vec3 dp1 = dFdx(v_pos), dp2 = dFdy(v_pos); vec2 du1 = dFdx(v_uv), du2 = dFdy(v_uv);
        vec3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
        vec3 T = dp2p * du1.x + dp1p * du2.x, B = dp2p * du1.y + dp1p * du2.y;
        float inv = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
        vec3 tn = texture(u_normal, v_uv).xyz * 2.0 - 1.0;
        if (u_mat == 1 && back) tn.xy = -tn.xy;
        N = normalize(T * inv * tn.x - B * inv * tn.y + N * tn.z);
    }
    if (u_mode == 1) { o = vec4(pow(albedo, vec3(1.0 / 2.2)), 1.0); return; }
    if (u_mode == 2) { o = vec4(N * 0.5 + 0.5, 1.0); return; }
    vec3 L = normalize(u_sun), V = normalize(u_eye - v_pos), H = normalize(L + V);
    float ndl = dot(N, L);
    vec3 sun = vec3(1.0, 0.96, 0.9) * 2.6;
    float diff = u_mat == 1 ? max((ndl + 0.25) / 1.25, 0.0) : max(ndl, 0.0);
    vec3 c = albedo * sun * diff;
    if (u_mat == 1) c += albedo * vec3(0.9, 1.0, 0.5) * sun * 0.35 * max(-ndl, 0.0);     // transmission when backlit
    float spec = pow(max(dot(N, H), 0.0), mix(64.0, 4.0, rough)) * (1.0 - rough) * 0.3;
    c += sun * spec * step(0.0, ndl);
    vec3 sky = mix(vec3(0.22, 0.2, 0.17), vec3(0.45, 0.55, 0.75), N.y * 0.5 + 0.5);
    c += albedo * sky * 0.55;
    c = 1.0 - exp(-c * 1.1);
    o = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}`;

const VS_LINE = `#version 300 es
layout(location=0) in vec3 a_pos; layout(location=1) in vec3 a_col;
uniform mat4 u_vp; out vec3 v_col;
void main() { v_col = a_col; gl_Position = u_vp * vec4(a_pos, 1.0); }`;
const FS_LINE = `#version 300 es
precision highp float; in vec3 v_col; out vec4 o; void main() { o = vec4(v_col, 1.0); }`;

const VS_QUAD = `#version 300 es
layout(location=0) in vec2 a_pos; uniform vec4 u_rect; out vec2 v_uv;
void main() { v_uv = vec2(a_pos.x, 1.0 - a_pos.y); gl_Position = vec4(u_rect.xy + a_pos * u_rect.zw, 0.0, 1.0); }`;
const FS_QUAD = `#version 300 es
precision highp float; in vec2 v_uv; uniform sampler2D u_tex; uniform int u_srgb; out vec4 o;
void main() { vec3 c = texture(u_tex, v_uv).rgb; if (u_srgb == 1) c = pow(c, vec3(1.0 / 2.2)); if (u_srgb == 2) c = vec3(c.g); o = vec4(c, 1.0); }`;

function gl_program(gl, vs, fs)
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
    const u = {};
    const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < n; i++) { const a = gl.getActiveUniform(p, i); u[a.name] = gl.getUniformLocation(p, a.name); }
    return { p: p, u: u };
}

function view_init(canvas)
{
    const gl = canvas.getContext('webgl2', { antialias: true, preserveDrawingBuffer: false });
    if (!gl) throw new Error('WebGL2 not available');
    const V = { gl: gl, canvas: canvas, az: 0.6, el: 0.25, dist: 40, target: [0, 10, 0], fit_height: 0,
                parts: [], lines: null, tex: { bark: null, leaf: null }, tex2d: null, mode: 0, wire: false };
    V.prog_mesh = gl_program(gl, VS_MESH, FS_MESH);
    V.prog_line = gl_program(gl, VS_LINE, FS_LINE);
    V.prog_quad = gl_program(gl, VS_QUAD, FS_QUAD);
    V.aniso = gl.getExtension('EXT_texture_filter_anisotropic');
    // Quad for texture previews.
    V.quad = gl.createVertexArray(); gl.bindVertexArray(V.quad);
    const qb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, qb);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    // Ground disc.
    const g = part_make('ground', 'ground'), segs = 64, R = 1;
    part_vertex(g, [0, 0, 0], [0, 1, 0], 0, 0, [1, 1, 1]);
    for (let i = 0; i <= segs; i++) { const a = i / segs * Math.PI * 2; part_vertex(g, [Math.cos(a) * R, 0, Math.sin(a) * R], [0, 1, 0], 0, 0, [1, 1, 1]); }
    for (let i = 0; i < segs; i++) g.idx.push(0, i + 2, i + 1);
    V.ground = view_upload_part(V, g);
    V.sphere = view_upload_part(V, solid_sphere());
    V.cube = view_upload_part(V, solid_cube());
    V.spin = 0;
    view_input(V);
    return V;
}

function view_upload_part(V, pt)
{
    const gl = V.gl, nv = pt.pos.length / 3;
    const f = new Float32Array(nv * 11);
    for (let i = 0, o = 0; i < nv; i++, o += 11) {
        f[o] = pt.pos[i * 3]; f[o + 1] = pt.pos[i * 3 + 1]; f[o + 2] = pt.pos[i * 3 + 2];
        f[o + 3] = pt.nrm[i * 3]; f[o + 4] = pt.nrm[i * 3 + 1]; f[o + 5] = pt.nrm[i * 3 + 2];
        f[o + 6] = pt.uv[i * 2]; f[o + 7] = pt.uv[i * 2 + 1];
        f[o + 8] = pt.col[i * 3]; f[o + 9] = pt.col[i * 3 + 1]; f[o + 10] = pt.col[i * 3 + 2];
    }
    const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
    const vb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, vb); gl.bufferData(gl.ARRAY_BUFFER, f, gl.STATIC_DRAW);
    const ib = gl.createBuffer(); gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(pt.idx), gl.STATIC_DRAW);
    const sz = [3, 3, 2, 3], off = [0, 3, 6, 8];
    for (let a = 0; a < 4; a++) { gl.enableVertexAttribArray(a); gl.vertexAttribPointer(a, sz[a], gl.FLOAT, false, 44, off[a] * 4); }
    gl.bindVertexArray(null);
    return { vao: vao, vb: vb, ib: ib, count: pt.idx.length, mat: pt.mat, lines: !!pt.lines };
}

function view_free_parts(V)
{
    const gl = V.gl;
    for (const p of V.parts) { gl.deleteVertexArray(p.vao); gl.deleteBuffer(p.vb); gl.deleteBuffer(p.ib); }
    V.parts = [];
    if (V.lines) { gl.deleteVertexArray(V.lines.vao); gl.deleteBuffer(V.lines.vb); V.lines = null; }
}

function view_set_mesh(V, mesh)
{
    view_free_parts(V);
    if (mesh) for (const p of mesh.parts) V.parts.push(view_upload_part(V, p));
}

// Skeleton as lines coloured by branch order; placements as short axis lines.
function view_set_lines(V, verts)
{
    view_free_parts(V);
    const gl = V.gl;
    const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
    const vb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, vb); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 24, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 24, 12);
    gl.bindVertexArray(null);
    V.lines = { vao: vao, vb: vb, count: verts.length / 6 };
}

const ORDER_COLORS = [[0.9, 0.75, 0.5], [0.95, 0.45, 0.35], [0.4, 0.8, 0.45], [0.4, 0.6, 0.95], [0.85, 0.5, 0.9], [0.9, 0.9, 0.4]];

function skeleton_lines(sk)
{
    const v = [];
    for (const b of sk.branches) {
        if (b.dead) continue;
        const c = ORDER_COLORS[b.order % ORDER_COLORS.length];
        for (let i = 1; i < b.pts.length; i++) v.push(...b.pts[i - 1], ...c, ...b.pts[i], ...c);
    }
    return v;
}

function placement_lines(pl)
{
    const v = [];
    for (const L of pl) {
        v.push(...L.p, 0.3, 0.8, 0.3, ...vmad(L.p, L.axis, 0.08 * L.s), 0.3, 0.8, 0.3);
        v.push(...L.p, 0.9, 0.9, 0.3, ...vmad(L.p, L.nrm, 0.03 * L.s), 0.9, 0.9, 0.3);
    }
    return v;
}

function gl_texture(V, rgba, size, srgb)
{
    const gl = V.gl, t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texImage2D(gl.TEXTURE_2D, 0, srgb ? gl.SRGB8_ALPHA8 : gl.RGBA8, size, size, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(rgba.buffer, rgba.byteOffset, rgba.length));
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
    if (V.aniso) gl.texParameterf(gl.TEXTURE_2D, V.aniso.TEXTURE_MAX_ANISOTROPY_EXT, 8);
    return t;
}

// Uploads a Texture (albedo/normal/mr) once per object; returns { albedo, normal, mr } GL textures.
function view_texture(V, t)
{
    if (!t) return null;
    if (t._gl && t._gl.gl === V.gl) return t._gl;
    t._gl = { gl: V.gl, albedo: gl_texture(V, t.albedo, t.size, true), normal: gl_texture(V, t.normal, t.size, false), mr: gl_texture(V, t.mr, t.size, false) };
    return t._gl;
}

function view_fit(V, height, radius)
{
    V.target = [0, height * 0.5, 0];
    V.dist = Math.max(height, radius * 2) * 1.5 + 0.05;
    V.fit_height = height;
}

function view_matrices(V, aspect)
{
    const ce = Math.cos(V.el);
    const eye = [V.target[0] + Math.sin(V.az) * ce * V.dist, V.target[1] + Math.sin(V.el) * V.dist, V.target[2] + Math.cos(V.az) * ce * V.dist];
    const proj = m4_perspective(40 * Math.PI / 180, aspect, V.dist * 0.01, V.dist * 20);
    return { vp: m4_mul(proj, m4_lookat(eye, V.target, [0, 1, 0])), eye: eye };
}

function view_draw_scene(V, w, h, bark, leaf)
{
    const gl = V.gl;
    gl.viewport(0, 0, w, h);
    gl.clearColor(0.16, 0.17, 0.19, 1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (V.tex2d && V.fullview) { view_draw_texture_full(V, w, h); return; }
    if (V.tex2d) { view_draw_tex2d(V, w, h); return; }
    gl.enable(gl.DEPTH_TEST);
    const M = view_matrices(V, w / Math.max(h, 1));
    const pm = V.prog_mesh;
    gl.useProgram(pm.p);
    gl.uniformMatrix4fv(pm.u.u_vp, false, M.vp);
    gl.uniformMatrix4fv(pm.u.u_model, false, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    gl.uniform3fv(pm.u.u_sun, [0.45, 0.8, 0.35]);
    gl.uniform3fv(pm.u.u_eye, M.eye);
    gl.uniform1i(pm.u.u_mode, V.mode);
    gl.uniform1i(pm.u.u_albedo, 0); gl.uniform1i(pm.u.u_normal, 1); gl.uniform1i(pm.u.u_mr, 2);
    // Ground disc scaled to the view.
    const gs = V.fit_height > 0 ? V.fit_height * 0.55 : 5;
    const ground_vp = m4_mul(M.vp, [gs, 0, 0, 0, 0, 1, 0, 0, 0, 0, gs, 0, 0, -0.002 * gs, 0, 1]);
    gl.uniformMatrix4fv(pm.u.u_vp, false, ground_vp);
    gl.uniform1i(pm.u.u_mat, 2); gl.uniform1i(pm.u.u_tex, 0); gl.uniform3fv(pm.u.u_base, [0.36, 0.34, 0.3]);
    gl.disable(gl.CULL_FACE);
    gl.bindVertexArray(V.ground.vao); gl.drawElements(gl.TRIANGLES, V.ground.count, gl.UNSIGNED_INT, 0);
    gl.uniformMatrix4fv(pm.u.u_vp, false, M.vp);
    gl.uniform1f(pm.u.u_bias, 0);
    for (const p of V.parts) {
        if (p.lines) {
            gl.uniform1i(pm.u.u_mat, 3); gl.uniform1f(pm.u.u_bias, 0.0004);
            gl.bindVertexArray(p.vao); gl.drawElements(gl.LINES, p.count, gl.UNSIGNED_INT, 0);
            gl.uniform1f(pm.u.u_bias, 0);
            continue;
        }
        const leafy = p.mat === 'leaves';
        const t = view_texture(V, leafy ? leaf : bark);
        gl.uniform1i(pm.u.u_mat, leafy ? 1 : 0);
        gl.uniform1i(pm.u.u_tex, t ? 1 : 0);
        gl.uniform3fv(pm.u.u_base, leafy ? [0.3, 0.45, 0.18] : [0.55, 0.53, 0.5]);
        if (t) {
            gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, t.albedo);
            gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, t.normal);
            gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, t.mr);
            gl.activeTexture(gl.TEXTURE0);
        }
        if (leafy) gl.disable(gl.CULL_FACE); else { gl.enable(gl.CULL_FACE); gl.cullFace(gl.BACK); }
        gl.bindVertexArray(p.vao);
        gl.drawElements(gl.TRIANGLES, p.count, gl.UNSIGNED_INT, 0);
    }
    if (V.lines) {
        const pl = V.prog_line;
        gl.useProgram(pl.p);
        gl.uniformMatrix4fv(pl.u.u_vp, false, M.vp);
        gl.bindVertexArray(V.lines.vao); gl.drawArrays(gl.LINES, 0, V.lines.count);
    }
    gl.bindVertexArray(null);
}

// Texture node preview: albedo, normal, roughness side by side.
function view_draw_tex2d(V, w, h)
{
    const gl = V.gl, t = view_texture(V, V.tex2d), q = V.prog_quad;
    gl.disable(gl.DEPTH_TEST);
    gl.useProgram(q.p);
    gl.bindVertexArray(V.quad);
    const s = Math.min(w / 3.2, h * 0.9), sx = s / w * 2, sy = s / h * 2;
    const maps = [t.albedo, t.normal, t.mr];
    for (let i = 0; i < 3; i++) {
        gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, maps[i]);
        gl.uniform1i(q.u.u_tex, 0); gl.uniform1i(q.u.u_srgb, i === 0 ? 1 : i === 2 ? 2 : 0);   // albedo, normal, roughness (g)
        gl.uniform4fv(q.u.u_rect, [-1 + (w - 3 * s) / w + i * sx * 1.0 + (i * 0.02), -sy / 2, sx * 0.97, sy]);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    gl.bindVertexArray(null);
}

// Full-screen texture preview: the three maps flat (top half), a lit rotating sphere and cube with the texture
// as material (bottom half).
function view_draw_texture_full(V, w, h)
{
    const gl = V.gl, h2 = Math.floor(h / 2);
    gl.viewport(0, h - h2, w, h2);
    view_draw_tex2d(V, w, h2);
    const t = view_texture(V, V.tex2d), pm = V.prog_mesh, a = V.spin;
    gl.enable(gl.DEPTH_TEST); gl.enable(gl.SCISSOR_TEST); gl.enable(gl.CULL_FACE); gl.cullFace(gl.BACK);
    gl.useProgram(pm.p);
    gl.uniform1i(pm.u.u_albedo, 0); gl.uniform1i(pm.u.u_normal, 1); gl.uniform1i(pm.u.u_mr, 2);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, t.albedo);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, t.normal);
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, t.mr);
    gl.activeTexture(gl.TEXTURE0);
    gl.uniform1i(pm.u.u_mat, 0); gl.uniform1i(pm.u.u_tex, 1); gl.uniform1i(pm.u.u_mode, V.mode);
    gl.uniform1f(pm.u.u_bias, 0); gl.uniform3fv(pm.u.u_base, [1, 1, 1]);
    gl.uniform3fv(pm.u.u_sun, [0.5, 0.7, 0.6]);
    const ca = Math.cos(a), sa = Math.sin(a), cb = Math.cos(0.45), sb = Math.sin(0.45);
    const model = m4_mul([1, 0, 0, 0, 0, cb, sb, 0, 0, -sb, cb, 0, 0, 0, 0, 1], [ca, 0, -sa, 0, 0, 1, 0, 0, sa, 0, ca, 0, 0, 0, 0, 1]);
    const objs = [V.sphere, V.cube];
    for (let k = 0; k < 2; k++) {
        const vw = Math.floor(w / 2), vx = k * vw;
        gl.viewport(vx, 0, vw, h2); gl.scissor(vx, 0, vw, h2);
        gl.clear(gl.DEPTH_BUFFER_BIT);
        const eye = [0, 0, 3.6];
        gl.uniformMatrix4fv(pm.u.u_vp, false, m4_mul(m4_perspective(40 * Math.PI / 180, vw / Math.max(h2, 1), 0.1, 20), m4_lookat(eye, [0, 0, 0], [0, 1, 0])));
        gl.uniformMatrix4fv(pm.u.u_model, false, model);
        gl.uniform3fv(pm.u.u_eye, eye);
        gl.bindVertexArray(objs[k].vao); gl.drawElements(gl.TRIANGLES, objs[k].count, gl.UNSIGNED_INT, 0);
    }
    gl.disable(gl.SCISSOR_TEST);
    gl.bindVertexArray(null);
}

// Unit sphere (uv: 2 texture repeats around, 1 from pole to pole) and cube (each face 0..1).
function solid_sphere()
{
    const pt = part_make('sphere', 'bark'), nu = 64, nv = 32, white = [1, 1, 1];
    for (let j = 0; j <= nv; j++)
        for (let i = 0; i <= nu; i++) {
            const th = j / nv * Math.PI, ph = i / nu * Math.PI * 2;
            const n = [Math.sin(th) * Math.cos(ph), Math.cos(th), Math.sin(th) * Math.sin(ph)];
            part_vertex(pt, n, n, i / nu * 2, j / nv, white);
        }
    for (let j = 0; j < nv; j++)
        for (let i = 0; i < nu; i++) { const a = j * (nu + 1) + i, b = a + nu + 1; pt.idx.push(a, a + 1, b + 1, a, b + 1, b); }
    return pt;
}

function solid_cube()
{
    const pt = part_make('cube', 'bark'), white = [1, 1, 1], s = 0.62;
    const faces = [[[1, 0, 0], [0, 0, -1], [0, 1, 0]], [[-1, 0, 0], [0, 0, 1], [0, 1, 0]], [[0, 1, 0], [1, 0, 0], [0, 0, -1]],
                   [[0, -1, 0], [1, 0, 0], [0, 0, 1]], [[0, 0, 1], [1, 0, 0], [0, 1, 0]], [[0, 0, -1], [-1, 0, 0], [0, 1, 0]]];
    for (const f of faces) {
        const n = f[0], u = f[1], v = f[2], base = pt.pos.length / 3;
        for (const c of [[0, 0], [1, 0], [1, 1], [0, 1]]) {
            const p = vscale(vadd(n, vadd(vscale(u, c[0] * 2 - 1), vscale(v, c[1] * 2 - 1))), s);
            part_vertex(pt, p, n, c[0], 1 - c[1], white);
        }
        pt.idx.push(base, base + 1, base + 2, base, base + 2, base + 3);
    }
    return pt;
}

function view_draw(V, bark, leaf)
{
    // Full-screen preview: slow turntable for 3D, rotating sphere / cube for textures.
    const now = performance.now(), dt = V.last_t ? Math.min(now - V.last_t, 100) / 1000 : 0;
    V.last_t = now;
    if (V.fullview && !V.auto_off) { V.spin = (V.spin || 0) + dt * 0.35; if (!V.tex2d) V.az += dt * 0.15; }
    const c = V.canvas, dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.floor(c.clientWidth * dpr)), h = Math.max(1, Math.floor(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    view_draw_scene(V, w, h, bark, leaf);
}

// Renders a mesh offscreen into an RGBA image (thumbnails).
function view_thumbnail(V, mesh, size, bark, leaf, height, radius)
{
    const gl = V.gl;
    if (!V.fbo || V.fbo.size !== size) {
        V.fbo = { size: size, fb: gl.createFramebuffer(), color: gl.createRenderbuffer(), depth: gl.createRenderbuffer() };
        gl.bindRenderbuffer(gl.RENDERBUFFER, V.fbo.color); gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, size, size);
        gl.bindRenderbuffer(gl.RENDERBUFFER, V.fbo.depth); gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT24, size, size);
        gl.bindFramebuffer(gl.FRAMEBUFFER, V.fbo.fb);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, V.fbo.color);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, V.fbo.depth);
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, V.fbo.fb);
    const saved = { parts: V.parts, lines: V.lines, tex2d: V.tex2d, target: V.target, dist: V.dist, fit: V.fit_height, mode: V.mode, wire: V.wire };
    V.parts = mesh.parts.map(p => view_upload_part(V, p)); V.lines = null; V.tex2d = null; V.mode = 0; V.wire = false;
    view_fit(V, height, radius);
    view_draw_scene(V, size, size, bark, leaf);
    const px = new Uint8Array(size * size * 4);
    gl.readPixels(0, 0, size, size, gl.RGBA, gl.UNSIGNED_BYTE, px);
    for (const p of V.parts) { gl.deleteVertexArray(p.vao); gl.deleteBuffer(p.vb); gl.deleteBuffer(p.ib); }
    V.parts = saved.parts; V.lines = saved.lines; V.tex2d = saved.tex2d; V.target = saved.target; V.dist = saved.dist;
    V.fit_height = saved.fit; V.mode = saved.mode; V.wire = saved.wire;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    const img = new ImageData(size, size);
    for (let y = 0; y < size; y++) img.data.set(px.subarray((size - 1 - y) * size * 4, (size - y) * size * 4), y * size * 4);
    return img;
}

function view_input(V)
{
    const c = V.canvas;
    let drag = null;
    c.addEventListener('contextmenu', e => e.preventDefault());
    c.addEventListener('mousedown', e => { drag = { x: e.clientX, y: e.clientY, b: e.button }; });
    window.addEventListener('mouseup', () => { drag = null; });
    window.addEventListener('mousemove', e => {
        if (!drag) return;
        const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
        drag.x = e.clientX; drag.y = e.clientY;
        // Turning by mouse stops the automatic rotation of the full-screen preview (until Space is pressed again).
        if (drag.b === 0) { V.auto_off = true; if (V.tex2d && V.fullview) V.spin -= dx * 0.008; else { V.az -= dx * 0.008; V.el = clamp(V.el + dy * 0.008, -1.4, 1.5); } }
        else {
            const s = V.dist * 0.0015;
            const right = [Math.cos(V.az), 0, -Math.sin(V.az)];
            V.target = vadd(vadd(V.target, vscale(right, -dx * s)), [0, dy * s, 0]);
        }
        V.dirty = true;
    });
    c.addEventListener('wheel', e => { e.preventDefault(); V.dist *= Math.pow(1.0015, e.deltaY); }, { passive: false });
}
