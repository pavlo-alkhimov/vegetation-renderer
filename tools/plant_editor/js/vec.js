// Small vector helpers on plain arrays [x, y, z]. Allocating versions; fast enough for editor-scale geometry.

function v3(x, y, z) { return [x, y, z]; }
function vadd(a, b) { return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]; }
function vsub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function vscale(a, s) { return [a[0] * s, a[1] * s, a[2] * s]; }
function vmad(a, b, s) { return [a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s]; }   // a + b * s
function vdot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function vcross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
function vlen(a) { return Math.sqrt(vdot(a, a)); }
function vnorm(a) { const l = vlen(a); return l > 1e-12 ? vscale(a, 1 / l) : [0, 1, 0]; }
function vlerp(a, b, t) { return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t]; }
function clamp(x, a, b) { return x < a ? a : x > b ? b : x; }
function lerp(a, b, t) { return a + (b - a) * t; }
function smoothstep(a, b, x) { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

// Any unit vector perpendicular to n.
function vperp(n)
{
    const a = Math.abs(n[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
    return vnorm(vcross(n, a));
}

// Rotate v around unit axis k by angle a (Rodrigues).
function vrot(v, k, a)
{
    const c = Math.cos(a), s = Math.sin(a);
    const kv = vcross(k, v), kd = vdot(k, v) * (1 - c);
    return [v[0] * c + kv[0] * s + k[0] * kd, v[1] * c + kv[1] * s + k[1] * kd, v[2] * c + kv[2] * s + k[2] * kd];
}

// Column-major 4x4 matrices for the viewport.
function m4_perspective(fovy, aspect, near, far)
{
    const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
    return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0];
}

function m4_lookat(eye, at, up)
{
    const z = vnorm(vsub(eye, at)), x = vnorm(vcross(up, z)), y = vcross(z, x);
    return [x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
            -vdot(x, eye), -vdot(y, eye), -vdot(z, eye), 1];
}

function m4_mul(a, b)
{
    const r = new Array(16);
    for (let c = 0; c < 4; c++)
        for (let rr = 0; rr < 4; rr++) {
            let s = 0;
            for (let k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    return r;
}
