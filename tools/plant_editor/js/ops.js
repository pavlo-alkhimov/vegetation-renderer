// Operator table: the single declaration of every node type. The editor rows, the .species format, hashing,
// variation by seed and the help text are all derived from it (JS counterpart of docs/15's X-macro table).
//
// Parameter: { name, type: 'f' | 'i' | 'e' | 'c', min, max, def, vari (default variation), step, labels, desc }.
// Seed channel: which instance seed drives the node ('structure' | 'foliage' | 'texture' | null).
// A float/int parameter's effective value = clamp(value + variation * u, min, max), u in [-1, 1) from the node's seed.
// A float/int parameter may hold one explicit value per LOD level (p.lods = [v0, v1, v2, v3]); the network's LOD input
// selects one. Nothing is derived from the LOD implicitly.

function F(name, min, max, def, vari, step, desc) { return { name: name, type: 'f', min: min, max: max, def: def, vari: vari, step: step, desc: desc }; }
function I(name, min, max, def, vari, desc) { return { name: name, type: 'i', min: min, max: max, def: def, vari: vari, step: 1, desc: desc }; }
function E(name, labels, def, desc) { return { name: name, type: 'e', labels: labels.split('|'), def: def, vari: 0, desc: desc }; }
function C(name, def, desc) { return { name: name, type: 'c', def: def, vari: 0, desc: desc }; }

const SIZES = '128|256|512|1024|2048';

const OPS = {
    Trunk: {
        chan: 'structure', ins: [], out: 'skeleton', ver: 1,
        desc: 'Main stem: a polyline with radius, taper and root flare.',
        params: [
            F('height', 0.1, 60, 24, 2, 0.1, 'stem height (m)'),
            F('radius', 0.02, 2, 0.33, 0.04, 0.01, 'base radius (m)'),
            F('taper', 0.3, 3, 1.1, 0.1, 0.05, 'radius exponent along the stem'),
            F('flare', 0, 2, 0.5, 0.15, 0.05, 'root flare at the base'),
            F('lean', 0, 30, 3, 2, 0.5, 'lean from vertical (deg)'),
            F('wobble', 0, 0.3, 0.025, 0.01, 0.005, 'random bending per segment'),
            I('segments', 2, 40, 12, 0, 'polyline segments'),
        ],
        fn: function (P, ins, seed) { return make_trunk(P, seed); },
    },
    Branches: {
        chan: 'structure', ins: ['skeleton'], out: 'skeleton', ver: 1, lod: true,
        desc: 'Adds one branch order on the highest existing order. Chain it once per order.',
        params: [
            F('count', 0, 200, 14, 2, 0.5, 'children on the longest parent (fewer on shorter ones)'),
            F('start', 0, 1, 0.3, 0.04, 0.01, 'first child along the parent (fraction)'),
            F('end', 0, 1, 0.95, 0, 0.01, 'last child along the parent (fraction)'),
            E('phyllotaxis', 'spiral|distichous|whorl', 0, 'arrangement around the parent'),
            I('whorl', 1, 12, 4, 0, 'children per whorl (whorl arrangement)'),
            F('angle', 0, 150, 55, 5, 1, 'angle from the parent (deg)'),
            F('angle_jitter', 0, 60, 10, 0, 1, 'random angle per child (deg)'),
            F('az_jitter', 0, 180, 25, 0, 1, 'random azimuth per child (deg)'),
            F('length', 0.02, 20, 0.45, 0.05, 0.01, 'child length / parent length at the first child'),
            F('length_top', 0, 2, 0.4, 0.05, 0.01, 'length factor of the last child relative to the first'),
            F('radius', 0.1, 1, 0.5, 0, 0.01, 'child base radius / parent radius at the attachment'),
            F('curvature', -2, 2, 0.3, 0.1, 0.05, 'upward bend over the child'),
            F('droop', 0, 2, 0.1, 0.05, 0.05, 'downward bend growing towards the tip'),
            F('wobble', 0, 0.3, 0.05, 0, 0.005, 'random bending per segment'),
            I('segments', 1, 20, 6, 0, 'polyline segments per child'),
            I('max_lod', 0, 3, 3, 0, 'highest LOD level that still has this branch order'),
        ],
        fn: function (P, ins, seed, ctx) { return ctx.lod > P.max_lod ? ins[0] : add_branches(ins[0], P, seed); },
    },
    Tropism: {
        chan: 'structure', ins: ['skeleton'], out: 'skeleton', ver: 1,
        desc: 'Bends branches down (gravity) or up (light), stronger towards tips and for higher orders.',
        params: [
            F('gravity', 0, 1, 0.08, 0.02, 0.01, 'downward bend'),
            F('photo', 0, 1, 0.03, 0.01, 0.01, 'upward bend'),
            F('order_scale', 0, 2, 0.6, 0, 0.05, 'exponent on the branch order'),
        ],
        fn: function (P, ins) { return apply_tropism(ins[0], P); },
    },
    Prune: {
        chan: 'structure', ins: ['skeleton'], out: 'skeleton', ver: 1,
        desc: 'Fits branches into a crown envelope; branches starting below the crown base are removed.',
        params: [
            E('shape', 'ellipsoid|egg|cone|cylinder', 1, 'crown envelope'),
            F('crown_base', 0, 0.9, 0.3, 0.04, 0.01, 'crown base / height'),
            F('width', 0.05, 1.5, 0.36, 0.04, 0.01, 'crown radius / height'),
            E('mode', 'cut|shrink', 1, 'cut at the envelope or shrink the branch to fit'),
            I('min_order', 1, 6, 1, 0, 'lowest order affected'),
            F('min_keep', 0, 0.9, 0.2, 0, 0.01, 'remove branches reduced below this fraction'),
        ],
        fn: function (P, ins) { return apply_prune(ins[0], P); },
    },
    LeafShape: {
        chan: 'foliage', ins: [], out: 'organ', ver: 1,
        desc: 'Leaf blade template: outline, margin, fold and curl, triangulated; also the texture mask.',
        params: [
            E('outline', 'ovate|elliptic|lobed|palmate|needle|spray|tuft', 0, 'outline; spray = conifer shoot with needles, tuft = needle bunch'),
            F('length', 0.01, 0.4, 0.09, 0.008, 0.005, 'blade length (m)'),
            F('width', 0.0005, 0.3, 0.055, 0.005, 0.0005, 'blade width (m); spray: 2 x needle length; tuft: needle base width'),
            F('petiole', 0, 0.1, 0.01, 0, 0.002, 'leaf stalk length (m)'),
            I('teeth', 0, 40, 7, 0, 'margin teeth (lobes for lobed; needles per side for spray, per bunch for tuft)'),
            F('tooth_depth', 0, 0.8, 0.06, 0, 0.01, 'tooth / lobe depth (spray: extra needle width / length)'),
            F('fold', 0, 1, 0.25, 0.05, 0.01, 'V-fold along the midrib (spray: needle lift; tuft: opening)'),
            F('curl', -1, 1, 0.15, 0.05, 0.01, 'bend along the blade (+ down)'),
            I('detail', 2, 12, 5, 0, 'segments along the blade'),
            I('veins', 2, 20, 7, 0, 'secondary vein pairs (texture)'),
        ],
        fn: function (P, ins, seed) { return make_leaf_shape(P, seed); },
    },
    Leaves: {
        chan: 'foliage', ins: ['skeleton', 'organ'], out: 'placements', ver: 1,
        desc: 'Places leaves on the highest branch orders.',
        params: [
            I('orders', 1, 4, 1, 0, 'number of highest branch orders carrying leaves'),
            F('density', 0, 80, 16, 2, 0.5, 'leaves per metre of branch'),
            F('start', 0, 1, 0.25, 0, 0.01, 'leaves from this fraction of the branch on'),
            E('arrangement', 'spiral|distichous', 1, 'leaf arrangement'),
            F('angle', 0, 120, 55, 5, 1, 'angle from the branch (deg)'),
            F('droop', -1, 1, 0.15, 0.05, 0.01, 'downward tilt of the leaf axis'),
            F('roll', 0, 90, 25, 0, 1, 'random roll around the leaf axis (deg)'),
            F('scale', 0.2, 12, 1.8, 0.1, 0.05, 'leaf scale (clusters at > 1)'),
            F('size_jitter', 0, 0.6, 0.2, 0, 0.01, 'random size per leaf'),
        ],
        fn: function (P, ins, seed) { return place_leaves(ins[0], ins[1], P, seed); },
    },
    Wood: {
        chan: 'structure', ins: ['skeleton'], out: 'mesh', ver: 1,
        desc: 'Tubes along all branches, cylindrical uv with constant texel density (material: bark).',
        params: [
            I('sides', 3, 24, 10, 0, 'tube sides at the trunk (fewer on thin branches)'),
            F('min_radius', 0.0005, 0.05, 0.003, 0, 0.0005, 'skip branches thinner than this (m)'),
            F('tile', 0.05, 3, 0.6, 0, 0.05, 'bark texture tile size (m)'),
        ],
        fn: function (P, ins) { return mesh_wood(ins[0], P); },
    },
    LeafMesh: {
        chan: 'foliage', ins: ['placements', 'organ'], out: 'mesh', ver: 1,
        desc: 'Leaf geometry at the placements; one connected component per leaf (material: leaves).',
        params: [
            F('color_var', 0, 0.5, 0.12, 0, 0.01, 'brightness variation per leaf (vertex colour)'),
            F('hue_var', 0, 0.3, 0.05, 0, 0.01, 'hue variation per leaf'),
        ],
        fn: function (P, ins) { return mesh_leaves(ins[0], ins[1], P); },
    },
    BarkTexture: {
        chan: 'texture', ins: [], out: 'texture', ver: 1, texture: true,
        desc: 'Tiling bark maps: albedo, normal, roughness. Furrows, plates, lenticels, moss.',
        params: [
            E('size', SIZES, 2, 'texture size (px)'),
            F('furrows', 1, 32, 6, 1, 1, 'furrows across the tile'),
            F('stretch', 0.1, 8, 3, 0, 0.1, 'vertical stretch of furrows and plates'),
            F('furrow_width', 0.01, 0.5, 0.08, 0.02, 0.01, 'furrow width'),
            F('furrow_depth', 0, 1, 0.12, 0.03, 0.01, 'furrow depth'),
            F('plates', 1, 40, 10, 2, 1, 'bark plates across the tile'),
            F('plate_depth', 0, 1, 0.08, 0, 0.01, 'plate relief'),
            F('lenticels', 1, 60, 18, 0, 1, 'lenticel cells across the tile'),
            F('lenticel_contrast', 0, 1, 0.35, 0, 0.01, 'lenticel visibility'),
            I('octaves', 1, 8, 5, 0, 'detail noise octaves'),
            F('moss', 0, 1, 0.15, 0.05, 0.01, 'moss cover'),
            F('roughness', 0, 1, 0.8, 0, 0.01, 'roughness'),
            F('normal_strength', 0, 4, 1, 0, 0.05, 'normal map strength'),
            C('color_dark', '#5f605b', 'dark bark colour'),
            C('color_light', '#a4a59e', 'light bark colour'),
            C('moss_color', '#5b6b2c', 'moss colour'),
            C('lenticel_color', '#4a4a46', 'lenticel and fissure colour'),
        ],
        fn: function (P, ins, seed) { return make_bark_texture(P, seed); },
    },
    LeafTexture: {
        chan: 'texture', ins: ['organ'], out: 'texture', ver: 1, texture: true,
        desc: 'Leaf maps over the blade uv box: albedo with veins, normal, roughness.',
        params: [
            E('size', SIZES, 1, 'texture size (px)'),
            C('color', '#3f6b1e', 'blade colour'),
            C('vein_color', '#93b055', 'vein colour'),
            F('vein_width', 0.005, 0.2, 0.035, 0, 0.005, 'midrib width / blade width'),
            F('vein_angle', 20, 80, 45, 3, 1, 'secondary vein angle (deg)'),
            F('vein_contrast', 0, 1, 0.45, 0.05, 0.01, 'vein visibility'),
            F('network', 2, 60, 18, 0, 1, 'tertiary vein cells along the blade'),
            F('color_var', 0, 0.5, 0.1, 0, 0.01, 'colour variation over the blade'),
            F('roughness', 0, 1, 0.55, 0, 0.01, 'roughness'),
            F('normal_strength', 0, 4, 0.6, 0, 0.05, 'normal map strength'),
        ],
        fn: function (P, ins, seed) { return make_leaf_texture(ins[0], P, seed); },
    },
    Wireframe: {
        chan: null, ins: ['mesh'], out: 'mesh', ver: 1,
        desc: 'Debug overlay: adds the triangle edges as a line part, shown in the viewport and exported as a separate line mesh (debug:wire) that the game viewer can switch on.',
        params: [
            E('enabled', 'off|on', 1, 'add the overlay'),
            E('parts', 'all|wood|leaves', 0, 'which parts get edges'),
            C('color', '#ffd040', 'line colour'),
        ],
        fn: function (P, ins) { return P.enabled ? mesh_wireframe(ins[0], P) : ins[0]; },
    },
    Merge: {
        chan: null, ins: ['mesh', 'mesh', 'mesh'], optional: [false, true, true], out: 'mesh', ver: 1,
        desc: 'Concatenates meshes; every part keeps its material.',
        params: [],
        fn: function (P, ins) { return mesh_merge(ins); },
    },
    Output: {
        chan: null, ins: ['mesh', 'texture', 'texture'], optional: [false, true, true], out: 'output', ver: 1,
        desc: 'The result: mesh + bark texture + leaf texture. Exported as glTF binary.',
        params: [],
        fn: function (P, ins) { return { mesh: ins[0], bark: ins[1], leaf: ins[2] }; },
    },
};

const OP_NAMES = Object.keys(OPS);

// Effective parameter values of a node for its seed.
function resolve_params(node, seed, lod)
{
    const op = OPS[node.op], P = {};
    for (let i = 0; i < op.params.length; i++) {
        const d = op.params[i], s = node.p[d.name] || { v: d.def, vari: d.vari };
        let v = s.lods ? s.lods[lod || 0] : s.v;
        if ((d.type === 'f' || d.type === 'i') && s.vari > 0) {
            const u = rng_s(rng_sub(seed, 1000 + i));
            v = clamp(v + s.vari * u, d.min, d.max);
        }
        if (d.type === 'i') v = Math.round(v);
        if (d.name === 'size' && d.type === 'e') v = parseInt(d.labels[v], 10);
        P[d.name] = v;
    }
    return P;
}

function node_defaults(op)
{
    const p = {};
    for (const d of OPS[op].params) p[d.name] = { v: d.def, vari: d.vari };
    return p;
}
