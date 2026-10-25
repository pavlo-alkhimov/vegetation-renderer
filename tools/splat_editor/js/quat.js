// Quaternions [w, x, y, z] (unit), conversions to and from rotation bases and Euler angles. Uses vec.js.

function q_ident() { return [1, 0, 0, 0]; }

function q_axis(axis, a)
{
    const s = Math.sin(a / 2), n = vnorm(axis);
    return [Math.cos(a / 2), n[0] * s, n[1] * s, n[2] * s];
}

function q_mul(a, b)
{
    return [a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]];
}

function q_norm(q)
{
    const l = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
    return [q[0] / l, q[1] / l, q[2] / l, q[3] / l];
}

// Rotation matrix columns = the rotated local axes.
function q_axes(q)
{
    const w = q[0], x = q[1], y = q[2], z = q[3];
    return [[1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)],
            [2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)],
            [2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)]];
}

function q_rotate(q, v)
{
    const a = q_axes(q);
    return [a[0][0] * v[0] + a[1][0] * v[1] + a[2][0] * v[2],
            a[0][1] * v[0] + a[1][1] * v[1] + a[2][1] * v[2],
            a[0][2] * v[0] + a[1][2] * v[1] + a[2][2] * v[2]];
}

// Quaternion of the orthonormal basis (local x, y, z axes in world space).
function q_from_axes(ax, ay, az)
{
    const m00 = ax[0], m11 = ay[1], m22 = az[2], tr = m00 + m11 + m22;
    let q;
    if (tr > 0) { const s = Math.sqrt(tr + 1) * 2; q = [0.25 * s, (ay[2] - az[1]) / s, (az[0] - ax[2]) / s, (ax[1] - ay[0]) / s]; }
    else if (m00 > m11 && m00 > m22) { const s = Math.sqrt(1 + m00 - m11 - m22) * 2; q = [(ay[2] - az[1]) / s, 0.25 * s, (ay[0] + ax[1]) / s, (az[0] + ax[2]) / s]; }
    else if (m11 > m22) { const s = Math.sqrt(1 + m11 - m00 - m22) * 2; q = [(az[0] - ax[2]) / s, (ay[0] + ax[1]) / s, 0.25 * s, (az[1] + ay[2]) / s]; }
    else { const s = Math.sqrt(1 + m22 - m00 - m11) * 2; q = [(ax[1] - ay[0]) / s, (az[0] + ax[2]) / s, (az[1] + ay[2]) / s, 0.25 * s]; }
    return q_norm(q);
}

// Euler angles in degrees, rotation order X then Y then Z (q = qz * qy * qx).
function q_to_euler(q)
{
    const w = q[0], x = q[1], y = q[2], z = q[3];
    const ex = Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y));
    const ey = Math.asin(clamp(2 * (w * y - z * x), -1, 1));
    const ez = Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
    return [ex * 180 / Math.PI, ey * 180 / Math.PI, ez * 180 / Math.PI];
}

function q_from_euler(e)
{
    const r = Math.PI / 180;
    return q_norm(q_mul(q_axis([0, 0, 1], e[2] * r), q_mul(q_axis([0, 1, 0], e[1] * r), q_axis([1, 0, 0], e[0] * r))));
}
