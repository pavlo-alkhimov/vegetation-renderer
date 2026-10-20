// glTF 2.0 binary (.glb) writer: one node + mesh per part ('wood', 'part:leaves'), materials 'bark' and 'leaves'
// (leaves double-sided), embedded PNG textures (baseColor, normal, metallicRoughness), metres, y up.
// extras carry species name, species hash and the seed vector, so every export traces back to (species, seeds).

function canvas_png(canvas)
{
    return new Promise(function (resolve) {
        canvas.toBlob(function (b) { b.arrayBuffer().then(ab => resolve(new Uint8Array(ab))); }, 'image/png');
    });
}

// Per-vertex tangents (xyz + handedness w) from triangle uv derivatives, orthonormalised against the normal.
function part_tangents(p)
{
    const nv = p.pos.length / 3, t = new Float32Array(nv * 3), b = new Float32Array(nv * 3), out = new Float32Array(nv * 4);
    for (let i = 0; i < p.idx.length; i += 3) {
        const i0 = p.idx[i], i1 = p.idx[i + 1], i2 = p.idx[i + 2];
        const e1 = [p.pos[i1 * 3] - p.pos[i0 * 3], p.pos[i1 * 3 + 1] - p.pos[i0 * 3 + 1], p.pos[i1 * 3 + 2] - p.pos[i0 * 3 + 2]];
        const e2 = [p.pos[i2 * 3] - p.pos[i0 * 3], p.pos[i2 * 3 + 1] - p.pos[i0 * 3 + 1], p.pos[i2 * 3 + 2] - p.pos[i0 * 3 + 2]];
        const du1 = p.uv[i1 * 2] - p.uv[i0 * 2], dv1 = p.uv[i1 * 2 + 1] - p.uv[i0 * 2 + 1];
        const du2 = p.uv[i2 * 2] - p.uv[i0 * 2], dv2 = p.uv[i2 * 2 + 1] - p.uv[i0 * 2 + 1];
        const det = du1 * dv2 - du2 * dv1;
        if (Math.abs(det) < 1e-12) continue;
        const r = 1 / det;
        for (const v of [i0, i1, i2])
            for (let k = 0; k < 3; k++) { t[v * 3 + k] += (e1[k] * dv2 - e2[k] * dv1) * r; b[v * 3 + k] += (e2[k] * du1 - e1[k] * du2) * r; }
    }
    for (let v = 0; v < nv; v++) {
        const n = [p.nrm[v * 3], p.nrm[v * 3 + 1], p.nrm[v * 3 + 2]];
        let tv = [t[v * 3], t[v * 3 + 1], t[v * 3 + 2]];
        tv = vsub(tv, vscale(n, vdot(n, tv)));
        tv = vlen(tv) > 1e-12 ? vnorm(tv) : vperp(n);
        const w = vdot(vcross(n, tv), [b[v * 3], b[v * 3 + 1], b[v * 3 + 2]]) < 0 ? -1 : 1;
        out[v * 4] = tv[0]; out[v * 4 + 1] = tv[1]; out[v * 4 + 2] = tv[2]; out[v * 4 + 3] = w;
    }
    return out;
}

async function gltf_export(out, extras)
{
    const json = {
        asset: { version: '2.0', generator: 'vegetation-renderer plant editor' },
        scene: 0, scenes: [{ nodes: [] }], nodes: [], meshes: [], materials: [], accessors: [], bufferViews: [],
        buffers: [], images: [], textures: [], samplers: [{ magFilter: 9729, minFilter: 9987, wrapS: 10497, wrapT: 10497 }],
        extras: extras,
    };
    const chunks = [];
    let offset = 0;
    const add_view = function (bytes, target) {
        const pad = (4 - (offset % 4)) % 4;
        if (pad) { chunks.push(new Uint8Array(pad)); offset += pad; }
        const bv = { buffer: 0, byteOffset: offset, byteLength: bytes.byteLength };
        if (target) bv.target = target;
        json.bufferViews.push(bv);
        chunks.push(new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength));
        offset += bytes.byteLength;
        return json.bufferViews.length - 1;
    };
    const add_accessor = function (arr, type, comp, target, minmax) {
        const view = add_view(arr, target);
        const n = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 }[type];
        const acc = { bufferView: view, componentType: comp, count: arr.length / n, type: type };
        if (minmax) {
            const mn = [Infinity, Infinity, Infinity].slice(0, n), mx = [-Infinity, -Infinity, -Infinity].slice(0, n);
            for (let i = 0; i < arr.length; i++) { const k = i % n; mn[k] = Math.min(mn[k], arr[i]); mx[k] = Math.max(mx[k], arr[i]); }
            acc.min = mn; acc.max = mx;
        }
        json.accessors.push(acc);
        return json.accessors.length - 1;
    };
    const add_image = async function (rgba, size) {
        const png = await canvas_png(rgba_canvas(rgba, size));
        json.images.push({ bufferView: add_view(png), mimeType: 'image/png' });
        json.textures.push({ sampler: 0, source: json.images.length - 1 });
        return json.textures.length - 1;
    };
    const mat_index = {};
    const add_material = async function (name, tex) {
        const m = { name: name, pbrMetallicRoughness: { metallicFactor: 0, roughnessFactor: 1 } };
        if (tex) {
            m.pbrMetallicRoughness.baseColorTexture = { index: await add_image(tex.albedo, tex.size) };
            m.pbrMetallicRoughness.metallicRoughnessTexture = { index: await add_image(tex.mr, tex.size) };
            m.normalTexture = { index: await add_image(tex.normal, tex.size) };
        } else m.pbrMetallicRoughness.baseColorFactor = name === 'leaves' ? [0.12, 0.25, 0.04, 1] : [0.3, 0.3, 0.28, 1];
        if (name === 'leaves') m.doubleSided = true;
        json.materials.push(m);
        mat_index[name] = json.materials.length - 1;
    };
    await add_material('bark', out.bark);
    await add_material('leaves', out.leaf);
    for (const p of out.mesh.parts) {
        if (!p.idx.length) continue;
        const prim = {
            attributes: {
                POSITION: add_accessor(new Float32Array(p.pos), 'VEC3', 5126, 34962, true),
                NORMAL: add_accessor(new Float32Array(p.nrm), 'VEC3', 5126, 34962),
                TANGENT: add_accessor(part_tangents(p), 'VEC4', 5126, 34962),
                TEXCOORD_0: add_accessor(new Float32Array(p.uv), 'VEC2', 5126, 34962),
                COLOR_0: add_accessor(new Float32Array(p.col.map(x => clamp(x, 0, 1))), 'VEC3', 5126, 34962),
            },
            indices: add_accessor(new Uint32Array(p.idx), 'SCALAR', 5125, 34963),
            material: mat_index[p.mat === 'leaves' ? 'leaves' : 'bark'],
        };
        json.meshes.push({ name: p.name, primitives: [prim] });
        json.nodes.push({ name: p.name, mesh: json.meshes.length - 1 });
        json.scenes[0].nodes.push(json.nodes.length - 1);
    }
    const pad_end = (4 - (offset % 4)) % 4;
    if (pad_end) { chunks.push(new Uint8Array(pad_end)); offset += pad_end; }
    json.buffers.push({ byteLength: offset });

    let js = new TextEncoder().encode(JSON.stringify(json));
    const jpad = (4 - (js.length % 4)) % 4;
    if (jpad) { const t = new Uint8Array(js.length + jpad).fill(0x20); t.set(js); js = t; }
    const total = 12 + 8 + js.length + 8 + offset;
    const glb = new Uint8Array(total), dv = new DataView(glb.buffer);
    dv.setUint32(0, 0x46546c67, true); dv.setUint32(4, 2, true); dv.setUint32(8, total, true);
    dv.setUint32(12, js.length, true); dv.setUint32(16, 0x4e4f534a, true); glb.set(js, 20);
    let o = 20 + js.length;
    dv.setUint32(o, offset, true); dv.setUint32(o + 4, 0x004e4942, true); o += 8;
    for (const c of chunks) { glb.set(c, o); o += c.length; }
    return glb;
}

function download(name, data, mime)
{
    const url = URL.createObjectURL(new Blob([data], { type: mime || 'application/octet-stream' }));
    const a = document.createElement('a');
    a.href = url; a.download = name;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}
