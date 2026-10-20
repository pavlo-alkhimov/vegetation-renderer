// .species files (the species = node graph + manually defined parameters) and .seeds files (instances).
//
// .species, one node per line (docs/15 recipe format):
//   name = Op @col,row in=a,b,- key=value key=value~variation ...
// '-' = unconnected input; variation after '~' (omitted when 0); enums by label; colours as #rrggbb; '#' comments.
// A number may be given per LOD level: key=v0|v1|v2|v3~variation. key=@node binds a parameter (or 'seed') to a value
// node; text values in double quotes: expr="2*x+1".
// .seeds, one instance per line: "structure foliage texture" (unsigned 32-bit integers).

function fmt_num(x)
{
    const s = (Math.round(x * 1e5) / 1e5).toString();
    return s;
}

function species_write(g, title)
{
    const lines = ['# ' + (title || 'species') + ' - plant editor species (graph + parameters; seeds are separate)'];
    const sorted = g.nodes.slice().sort((a, b) => a.row - b.row || a.col - b.col);
    for (const n of sorted) {
        const op = OPS[n.op];
        let s = n.name + ' = ' + n.op + ' @' + n.col + ',' + n.row;
        if (op.ins.length) s += ' in=' + n.ins.map(id => { const m = node_find(g, id); return m ? m.name : '-'; }).join(',');
        for (const d of op.params) {
            const p = n.p[d.name];
            let v;
            if (n.binds[d.name]) { s += ' ' + d.name + '=@' + node_find(g, n.binds[d.name]).name; continue; }
            if (d.type === 'e') v = d.labels[p.v];
            else if (d.type === 's') v = '"' + p.v.replace(/"/g, "'") + '"';
            else if (d.type === 'c') v = p.v;
            else v = (p.lods ? p.lods.map(fmt_num).join('|') : fmt_num(p.v)) + (p.vari > 0 ? '~' + fmt_num(p.vari) : '');
            s += ' ' + d.name + '=' + v;
        }
        if (n.binds.seed) s += ' seed=@' + node_find(g, n.binds.seed).name;
        lines.push(s);
    }
    return lines.join('\n') + '\n';
}

// Returns { graph, errors }.
function species_read(text)
{
    const g = graph_new(), errors = [], pending = [];
    const lines = text.split(/\r?\n/);
    for (let ln = 0; ln < lines.length; ln++) {
        const line = lines[ln].replace(/#[^0-9a-fA-F].*$|^\s*#.*$/, '').trim();
        if (!line) continue;
        const m = line.match(/^([A-Za-z_][\w]*)\s*=\s*([A-Za-z]\w*)\s*(.*)$/);
        if (!m) { errors.push('line ' + (ln + 1) + ': cannot parse'); continue; }
        const name = m[1], opn = m[2];
        if (!OPS[opn]) { errors.push('line ' + (ln + 1) + ': unknown operator ' + opn); continue; }
        if (node_by_name(g, name)) { errors.push('line ' + (ln + 1) + ': duplicate name ' + name); continue; }
        const n = node_add(g, opn, 0, 0, name);
        const op = OPS[opn];
        for (const tok of (m[3].match(/\S+?="[^"]*"|\S+/g) || [])) {
            if (tok[0] === '@') {
                const c = tok.slice(1).split(',');
                n.col = parseInt(c[0], 10) || 0; n.row = parseInt(c[1], 10) || 0;
                continue;
            }
            const eq = tok.indexOf('=');
            if (eq < 0) { errors.push('line ' + (ln + 1) + ': bad token ' + tok); continue; }
            const key = tok.slice(0, eq), val = tok.slice(eq + 1);
            if (key === 'in') { pending.push({ node: n, refs: val.split(','), ln: ln + 1 }); continue; }
            if (val[0] === '@') { pending.push({ node: n, bind: key, ref: val.slice(1), ln: ln + 1 }); continue; }
            const d = op.params.find(x => x.name === key);
            if (!d) { errors.push('line ' + (ln + 1) + ': ' + opn + ' has no parameter ' + key); continue; }
            if (d.type === 's') { n.p[key].v = val.replace(/^"|"$/g, ''); continue; }
            if (d.type === 'e') {
                const k = d.labels.indexOf(val);
                if (k < 0) errors.push('line ' + (ln + 1) + ': ' + key + ' must be one of ' + d.labels.join('|'));
                else n.p[key].v = k;
            } else if (d.type === 'c') {
                if (/^#[0-9a-fA-F]{6}$/.test(val)) n.p[key].v = val.toLowerCase();
                else errors.push('line ' + (ln + 1) + ': ' + key + ' must be #rrggbb');
            } else {
                const parts = val.split('~');
                // value, or 4 values separated by '|' (one per LOD level)
                const vals = parts[0].split('|').map(parseFloat), vari = parts.length > 1 ? parseFloat(parts[1]) : 0;
                if (vals.some(isNaN) || isNaN(vari)) { errors.push('line ' + (ln + 1) + ': bad number ' + val); continue; }
                if (vals.length !== 1 && vals.length !== 4) { errors.push('line ' + (ln + 1) + ': ' + key + ' needs 1 value or 4 (per LOD)'); continue; }
                if (vals.some(v => v < d.min || v > d.max)) errors.push('line ' + (ln + 1) + ': ' + key + ' outside ' + d.min + '..' + d.max + ' (clamped)');
                const cv = vals.map(v => { v = clamp(v, d.min, d.max); return d.type === 'i' ? Math.round(v) : v; });
                n.p[key] = { v: cv[0], vari: Math.max(0, vari) };
                if (cv.length === 4) n.p[key].lods = cv;
            }
        }
    }
    for (const p of pending) {
        if (p.bind) {
            const src = node_by_name(g, p.ref);
            if (!src) { errors.push('line ' + p.ln + ': unknown node @' + p.ref); continue; }
            const e = bind(g, src.id, p.node.id, p.bind);
            if (e) errors.push('line ' + p.ln + ': ' + e);
            continue;
        }
        for (let i = 0; i < p.refs.length && i < p.node.ins.length; i++) {
            if (p.refs[i] === '-' || p.refs[i] === '') continue;
            const src = node_by_name(g, p.refs[i]);
            if (!src) { errors.push('line ' + p.ln + ': unknown input ' + p.refs[i]); continue; }
            const e = connect(g, src.id, p.node.id, i);
            if (e) errors.push('line ' + p.ln + ': ' + e);
        }
    }
    // Two nodes in one cell: move the later one to the nearest free cell.
    const seen = new Set();
    for (const n of g.nodes) {
        const k = n.col + ',' + n.row;
        if (seen.has(k)) { const c = nearest_free_cell(g, n.col, n.row); n.col = c[0]; n.row = c[1]; }
        seen.add(n.col + ',' + n.row);
    }
    return { graph: g, errors: errors };
}

function seeds_write(list)
{
    return '# structure foliage texture\n' + list.map(s => s.structure + ' ' + s.foliage + ' ' + s.texture).join('\n') + '\n';
}

function seeds_read(text)
{
    const out = [];
    for (const raw of text.split(/\r?\n/)) {
        const line = raw.replace(/#.*$/, '').trim();
        if (!line) continue;
        const v = line.split(/\s+/).map(x => parseInt(x, 10) >>> 0);
        out.push({ structure: v[0] || 0, foliage: v[1] || 0, texture: v[2] || 0 });
    }
    return out;
}

// Built-in species library (Central European trees and a shrub). All use the same table layout:
// row 0 structure (trunk, branch orders, tropism, prune, wood, merge, wireframe, output), row 1 foliage, row 2 textures.
// Built-in species library (Central European trees and a shrub). Shared table layout:
//   column 0: network inputs (lod, the three seeds, three constants); column 1: derived values (expressions);
//   columns 2..: structure (row 0), foliage (row 1), textures (row 2).
// Every node's seed is bound explicitly to its channel's Seed node; the trunk's vertical and radial segment counts
// come from const1 / const2 through LOD expressions.
function make_species(title, n)
{
    const L = ['# ' + title + ' - plant editor species (graph + parameters; seeds are separate)'];
    const seg0 = parseInt((n.trunk.match(/segments=(\d+)/) || [0, 12])[1], 10);
    const sides0 = Math.max(3, parseInt((n.wood.match(/sides=(\d+)/) || [0, 10])[1], 10) + 2);
    L.push('lod = Lod @0,0');
    L.push('seed_structure = Seed @0,1 channel=structure');
    L.push('seed_foliage = Seed @0,2 channel=foliage');
    L.push('seed_texture = Seed @0,3 channel=texture');
    L.push('const1 = Const @0,4 value=' + seg0);
    L.push('const2 = Const @0,5 value=' + sides0);
    L.push('const3 = Const @0,6 value=1');
    L.push('trunk_segments = Expr @1,4 in=const1,lod,- expr="max(2, round(x*(1-0.17*y)))"');
    L.push('trunk_sides = Expr @1,5 in=const2,lod,- expr="max(3, round(x*pow(0.65,y)))"');
    const S = ' seed=@seed_structure', Fo = ' seed=@seed_foliage', T = ' seed=@seed_texture';
    L.push('trunk = Trunk @2,0 ' + n.trunk.replace(/segments=\S+/, 'segments=@trunk_segments sides=@trunk_sides') + S);
    let prev = 'trunk';
    n.branches.forEach(function (b, i) { L.push('branches' + (i + 1) + ' = Branches @' + (i + 3) + ',0 in=' + prev + ' ' + b + S); prev = 'branches' + (i + 1); });
    L.push('tropism = Tropism @6,0 in=' + prev + ' ' + n.tropism + S);
    L.push('prune = Prune @7,0 in=tropism ' + n.prune + S);
    L.push('wood = Wood @8,0 in=prune ' + n.wood + S);
    L.push('merge = Merge @9,0 in=wood,leafmesh,-');
    L.push('wireframe = Wireframe @10,0 in=merge enabled=off parts=all color=#ffd040');
    L.push('output = Output @11,0 in=wireframe,barktexture,leaftexture');
    L.push('leafshape = LeafShape @6,1 ' + n.leafshape + Fo);
    L.push('leaves = Leaves @7,1 in=prune,leafshape ' + n.leaves + Fo);
    L.push('leafmesh = LeafMesh @8,1 in=leaves,leafshape ' + n.leafmesh + Fo);
    L.push('leaftexture = LeafTexture @8,2 in=leafshape ' + n.leaftexture + T);
    L.push('barktexture = BarkTexture @9,2 ' + n.bark + T);
    return L.join('\n') + '\n';
}

const SPECIES_LIBRARY = {
    // European beech (Fagus sylvatica): tall clean bole, broad dome, distichous twigs, smooth grey bark.
    beech_like: make_species('beech_like', {
        trunk: 'height=24~2 radius=0.33~0.04 taper=1.1~0.1 flare=0.5~0.15 lean=3~2 wobble=0.025~0.01 segments=12|10|8|6',
        branches: [
            'count=18~2 start=0.3~0.03 end=0.97 phyllotaxis=spiral angle=62~5 angle_jitter=10 az_jitter=25 length=0.55~0.05 length_top=0.3~0.05 radius=0.5 curvature=0.25~0.08 droop=0.06~0.03 wobble=0.06 segments=7 max_lod=3',
            'count=11~1.5 start=0.12 end=0.95 phyllotaxis=distichous angle=52~5 angle_jitter=12 az_jitter=35 length=0.5~0.05 length_top=0.5~0.05 radius=0.55 curvature=0.12~0.05 droop=0.18~0.05 wobble=0.07 segments=5 max_lod=2',
            'count=8~1 start=0.1 end=0.95 phyllotaxis=distichous angle=42~4 angle_jitter=12 az_jitter=40 length=0.42~0.04 length_top=0.6 radius=0.6 curvature=0.05 droop=0.1~0.05 wobble=0.08 segments=3 max_lod=1',
        ],
        tropism: 'gravity=0.08~0.02 photo=0.03~0.01 order_scale=0.6',
        prune: 'shape=ellipsoid crown_base=0.28~0.04 width=0.4~0.04 mode=shrink min_order=1 min_keep=0.2',
        wood: 'sides=10|7|4|3 min_radius=0.003|0.005|0.007|0.011 tile=0.6',
        leafshape: 'outline=ovate length=0.09~0.008 width=0.055~0.005 petiole=0.01 teeth=7 tooth_depth=0.06 fold=0.25~0.05 curl=0.15~0.05 detail=4|3|2|2 veins=7',
        leaves: 'orders=2 density=9|4.5|2.25|1.125~1.5 start=0.3 arrangement=distichous angle=55~5 droop=0.15~0.05 roll=25 scale=2.2|3.111|4.4|6.223~0.1 size_jitter=0.2',
        leafmesh: 'color_var=0.12 hue_var=0.05',
        leaftexture: 'size=256 color=#3f6b1e vein_color=#93b055 vein_width=0.035 vein_angle=45~3 vein_contrast=0.45~0.05 network=18 color_var=0.1 roughness=0.55 normal_strength=0.6',
        bark: 'size=512 furrows=6~1 stretch=3 furrow_width=0.08~0.02 furrow_depth=0.12~0.03 plates=10~2 plate_depth=0.08 lenticels=18 lenticel_contrast=0.35 octaves=5 moss=0.15~0.05 roughness=0.8 normal_strength=1 color_dark=#5f605b color_light=#a4a59e moss_color=#5b6b2c lenticel_color=#4a4a46',
    }),
    // Norway spruce (Picea abies): narrow cone to the ground, whorled near-horizontal branches with hanging
    // side shoots and upturned tips, needle sprays, red-brown scaly bark.
    spruce: make_species('spruce', {
        trunk: 'height=28~2 radius=0.34~0.04 taper=1~0.1 flare=0.4~0.1 lean=1~1 wobble=0.01 segments=16|13|10|8',
        branches: [
            'count=150~10 start=0.06 end=0.99 phyllotaxis=whorl whorl=5 angle=86~5 angle_jitter=8 az_jitter=15 length=0.2~0.02 length_top=0.1 radius=0.3 curvature=0.45~0.1 droop=0.35~0.1 wobble=0.03 segments=6 max_lod=3',
            'count=14~2 start=0.1 end=0.95 phyllotaxis=distichous angle=62~5 angle_jitter=12 az_jitter=25 length=0.32~0.04 length_top=0.45 radius=0.45 curvature=0 droop=0.45~0.1 wobble=0.05 segments=3 max_lod=1',
        ],
        tropism: 'gravity=0.05~0.02 photo=0.02 order_scale=0.5',
        prune: 'shape=cone crown_base=0.04~0.02 width=0.22~0.02 mode=shrink min_order=1 min_keep=0.15',
        wood: 'sides=8|5|3|3 min_radius=0.004|0.006|0.009|0.015 tile=0.4',
        leafshape: 'outline=spray length=0.3~0.03 width=0.11 petiole=0 teeth=6 tooth_depth=0.16 fold=0.35~0.05 curl=0.2~0.05 detail=4 veins=2',
        leaves: 'orders=2 density=3.6|1.8|0.9|0.45~0.4 start=0.1 arrangement=distichous angle=45~5 droop=0.1 roll=40 scale=1.5|2.121|3|4.243~0.1 size_jitter=0.2',
        leafmesh: 'color_var=0.08 hue_var=0.03',
        leaftexture: 'size=128 color=#2e5226 vein_color=#3b6630 vein_width=0.03 vein_angle=45 vein_contrast=0.15 network=30 color_var=0.12 roughness=0.5 normal_strength=0.3',
        bark: 'size=512 furrows=4 stretch=1 furrow_width=0.05 furrow_depth=0.06 plates=22~3 plate_depth=0.25 lenticels=1 lenticel_contrast=0 octaves=5 moss=0.1~0.05 roughness=0.85 normal_strength=1.2 color_dark=#45362b color_light=#86705f moss_color=#5b6b2c lenticel_color=#2e241d',
    }),
    // Scots pine (Pinus sylvestris): tall bare bole, high irregular flat-topped crown, needle pairs in tufts at the
    // shoot ends, grey-brown furrowed bark below, orange plates above.
    scots_pine: make_species('scots_pine', {
        trunk: 'height=24~2 radius=0.3~0.03 taper=1.2~0.1 flare=0.4~0.1 lean=4~2 wobble=0.04~0.01 segments=14|11|9|7',
        branches: [
            'count=34~4 start=0.58~0.04 end=0.98 phyllotaxis=whorl whorl=4 angle=78~8 angle_jitter=15 az_jitter=30 length=0.26~0.03 length_top=0.5 radius=0.38 curvature=0.22~0.08 droop=0.15~0.05 wobble=0.1 segments=6 max_lod=3',
            'count=7~1 start=0.3 end=1 phyllotaxis=spiral angle=45~5 angle_jitter=15 az_jitter=40 length=0.42~0.05 length_top=0.6 radius=0.5 curvature=0.4 droop=0.05 wobble=0.1 segments=4 max_lod=2',
            'count=4~1 start=0.4 end=1 phyllotaxis=spiral angle=40 angle_jitter=15 az_jitter=40 length=0.4 length_top=0.7 radius=0.6 curvature=0.3 droop=0 wobble=0.1 segments=2 max_lod=1',
        ],
        tropism: 'gravity=0.04 photo=0.03~0.01 order_scale=0.6',
        prune: 'shape=ellipsoid crown_base=0.6~0.04 width=0.26~0.03 mode=shrink min_order=1 min_keep=0.15',
        wood: 'sides=9|6|4|3 min_radius=0.004|0.006|0.009|0.015 tile=0.5',
        leafshape: 'outline=tuft length=0.09~0.01 width=0.012 petiole=0 teeth=10 tooth_depth=0 fold=0.6~0.1 curl=0 detail=4 veins=2',
        leaves: 'orders=2 density=18|9|4.5|2.25~2 start=0.35 arrangement=spiral angle=30~5 droop=-0.2 roll=0 scale=2.2|3.111|4.4|6.223~0.1 size_jitter=0.15',
        leafmesh: 'color_var=0.1 hue_var=0.05',
        leaftexture: 'size=128 color=#3c5a2a vein_color=#4a6a34 vein_width=0.03 vein_angle=45 vein_contrast=0.1 network=30 color_var=0.1 roughness=0.45 normal_strength=0.2',
        bark: 'size=512 furrows=5~1 stretch=2 furrow_width=0.1 furrow_depth=0.3~0.05 plates=12~2 plate_depth=0.3 lenticels=1 lenticel_contrast=0 octaves=5 moss=0.08 roughness=0.85 normal_strength=1.3 color_dark=#4b3426 color_light=#b0703f moss_color=#5b6b2c lenticel_color=#2b1f18',
    }),
    // European larch (Larix decidua): deciduous conifer, open cone, spirally set branches with drooping side
    // shoots, soft light-green needle bunches on short shoots, red-brown furrowed bark.
    larch: make_species('larch', {
        trunk: 'height=26~2 radius=0.3~0.03 taper=1~0.1 flare=0.4 lean=2~1 wobble=0.02 segments=14|11|9|7',
        branches: [
            'count=60~6 start=0.15~0.03 end=0.99 phyllotaxis=spiral angle=80~6 angle_jitter=12 az_jitter=20 length=0.22~0.02 length_top=0.15 radius=0.34 curvature=0.3~0.1 droop=0.25~0.08 wobble=0.05 segments=6 max_lod=3',
            'count=11~2 start=0.1 end=0.95 phyllotaxis=distichous angle=55~5 angle_jitter=15 az_jitter=30 length=0.4~0.04 length_top=0.5 radius=0.45 curvature=0 droop=0.75~0.15 wobble=0.06 segments=4 max_lod=1',
        ],
        tropism: 'gravity=0.1~0.03 photo=0.02 order_scale=0.8',
        prune: 'shape=cone crown_base=0.15~0.03 width=0.2~0.02 mode=shrink min_order=1 min_keep=0.15',
        wood: 'sides=8|5|3|3 min_radius=0.004|0.006|0.009|0.015 tile=0.45',
        leafshape: 'outline=tuft length=0.045~0.005 width=0.009 petiole=0 teeth=12 tooth_depth=0 fold=0.75~0.1 curl=0 detail=4 veins=2',
        leaves: 'orders=2 density=13|6.5|3.25|1.625~1.5 start=0.1 arrangement=spiral angle=65~5 droop=0 roll=0 scale=2.6|3.677|5.2|7.354~0.1 size_jitter=0.15',
        leafmesh: 'color_var=0.08 hue_var=0.04',
        leaftexture: 'size=128 color=#6f9e3c vein_color=#7fab4a vein_width=0.03 vein_angle=45 vein_contrast=0.1 network=30 color_var=0.1 roughness=0.5 normal_strength=0.2',
        bark: 'size=512 furrows=6~1 stretch=2 furrow_width=0.1 furrow_depth=0.3~0.05 plates=16~2 plate_depth=0.25 lenticels=1 lenticel_contrast=0 octaves=5 moss=0.1 roughness=0.85 normal_strength=1.2 color_dark=#3d2a22 color_light=#8e5c47 moss_color=#5b6b2c lenticel_color=#2a1c16',
    }),
    // Silver birch (Betula pendula): slender, narrow crown, long pendulous twigs, small serrate triangular-ovate
    // leaves, white bark with dark horizontal lenticels and fissures.
    silver_birch: make_species('silver_birch', {
        trunk: 'height=20~2 radius=0.2~0.03 taper=1~0.1 flare=0.3 lean=4~2 wobble=0.03~0.01 segments=12|10|8|6',
        branches: [
            'count=24~3 start=0.3~0.04 end=0.97 phyllotaxis=spiral angle=42~5 angle_jitter=10 az_jitter=30 length=0.36~0.04 length_top=0.4 radius=0.45 curvature=0.5~0.1 droop=0.12~0.05 wobble=0.06 segments=6 max_lod=3',
            'count=10~1.5 start=0.2 end=0.97 phyllotaxis=distichous angle=45~5 angle_jitter=15 az_jitter=35 length=0.5~0.05 length_top=0.6 radius=0.5 curvature=0 droop=0.6~0.15 wobble=0.06 segments=5 max_lod=2',
            'count=6~1 start=0.1 end=0.97 phyllotaxis=distichous angle=30~5 angle_jitter=10 az_jitter=40 length=0.55~0.05 length_top=0.7 radius=0.6 curvature=0 droop=1.2~0.2 wobble=0.04 segments=4 max_lod=1',
        ],
        tropism: 'gravity=0.25~0.05 photo=0 order_scale=1',
        prune: 'shape=egg crown_base=0.3~0.05 width=0.26~0.03 mode=shrink min_order=1 min_keep=0.2',
        wood: 'sides=8|5|3|3 min_radius=0.003|0.004|0.006|0.009 tile=0.5',
        leafshape: 'outline=ovate length=0.05~0.005 width=0.04~0.004 petiole=0.015 teeth=14 tooth_depth=0.12 fold=0.15~0.05 curl=0.1 detail=4|3|2|2 veins=6',
        leaves: 'orders=2 density=12|6|3|1.5~1.5 start=0.2 arrangement=spiral angle=50~5 droop=0.3~0.1 roll=35 scale=2.2|3.111|4.4|6.223~0.1 size_jitter=0.2',
        leafmesh: 'color_var=0.12 hue_var=0.06',
        leaftexture: 'size=256 color=#4f7d25 vein_color=#8fb256 vein_width=0.03 vein_angle=42~3 vein_contrast=0.4 network=16 color_var=0.1 roughness=0.5 normal_strength=0.6',
        bark: 'size=512 furrows=3 stretch=0.3 furrow_width=0.04 furrow_depth=0.08~0.03 plates=3 plate_depth=0.02 lenticels=9~1 lenticel_contrast=0.9 octaves=5 moss=0.05 roughness=0.6 normal_strength=0.6 color_dark=#c9c5b9 color_light=#f0ede4 moss_color=#7a7f50 lenticel_color=#2b2926',
    }),
    // Pedunculate oak (Quercus robur): short massive bole, wide irregular crown of crooked branches, lobed
    // leaves on short stalks, deeply furrowed dark bark.
    oak: make_species('oak', {
        trunk: 'height=18~2 radius=0.45~0.05 taper=1.3~0.1 flare=0.6~0.15 lean=5~3 wobble=0.05~0.02 segments=12|10|8|6',
        branches: [
            'count=12~2 start=0.25~0.04 end=0.9 phyllotaxis=spiral angle=60~8 angle_jitter=20 az_jitter=40 length=0.62~0.06 length_top=0.5 radius=0.55 curvature=0.2~0.08 droop=0.15~0.05 wobble=0.15~0.03 segments=8 max_lod=3',
            'count=10~1.5 start=0.1 end=0.95 phyllotaxis=spiral angle=55~6 angle_jitter=20 az_jitter=40 length=0.45~0.05 length_top=0.55 radius=0.5 curvature=0.1 droop=0.2~0.05 wobble=0.15 segments=6 max_lod=2',
            'count=7~1 start=0.1 end=0.95 phyllotaxis=spiral angle=50~5 angle_jitter=20 az_jitter=40 length=0.4~0.04 length_top=0.6 radius=0.6 curvature=0.05 droop=0.1 wobble=0.15 segments=3 max_lod=1',
        ],
        tropism: 'gravity=0.06~0.02 photo=0.03 order_scale=0.6',
        prune: 'shape=ellipsoid crown_base=0.2~0.04 width=0.55~0.05 mode=shrink min_order=1 min_keep=0.2',
        wood: 'sides=11|7|5|3 min_radius=0.003|0.005|0.007|0.011 tile=0.5',
        leafshape: 'outline=lobed length=0.1~0.01 width=0.055~0.005 petiole=0.005 teeth=5 tooth_depth=0.5~0.05 fold=0.1 curl=0.1 detail=6|5|4|3 veins=6',
        leaves: 'orders=2 density=11|5.5|2.75|1.375~1.5 start=0.3 arrangement=spiral angle=50~5 droop=0.1 roll=30 scale=1.9|2.687|3.8|5.374~0.1 size_jitter=0.2',
        leafmesh: 'color_var=0.1 hue_var=0.05',
        leaftexture: 'size=256 color=#3a5e1d vein_color=#7c9a46 vein_width=0.035 vein_angle=50~3 vein_contrast=0.4 network=18 color_var=0.1 roughness=0.6 normal_strength=0.6',
        bark: 'size=512 furrows=9~1 stretch=2.5 furrow_width=0.15~0.03 furrow_depth=0.5~0.05 plates=14~2 plate_depth=0.3 lenticels=1 lenticel_contrast=0 octaves=6 moss=0.25~0.05 roughness=0.9 normal_strength=1.6 color_dark=#2e2925 color_light=#73695d moss_color=#55652a lenticel_color=#1e1a17',
    }),
    // Common hazel (Corylus avellana): multi-stemmed shrub ~5 m, arching stems, round doubly serrate leaves,
    // smooth shiny grey-brown bark with lenticels.
    hazel: make_species('hazel', {
        trunk: 'height=0.6 radius=0.07 taper=0.5 flare=0 lean=0 wobble=0 segments=2',
        branches: [
            'count=9~2 start=0.05 end=0.95 phyllotaxis=spiral angle=16~4 angle_jitter=8 az_jitter=40 length=8~1 length_top=0.8 radius=0.85 curvature=-0.06~0.04 droop=0.1 wobble=0.05 segments=8 max_lod=3',
            'count=12~2 start=0.25 end=0.97 phyllotaxis=spiral angle=45~5 angle_jitter=15 az_jitter=40 length=0.32~0.04 length_top=0.5 radius=0.4 curvature=0.1 droop=0.3~0.1 wobble=0.08 segments=4 max_lod=2',
            'count=6~1 start=0.1 end=0.95 phyllotaxis=distichous angle=45 angle_jitter=15 az_jitter=40 length=0.4 length_top=0.6 radius=0.5 curvature=0 droop=0.15 wobble=0.08 segments=2 max_lod=1',
        ],
        tropism: 'gravity=0.06 photo=0.04 order_scale=0.5',
        prune: 'shape=cylinder crown_base=0 width=0.42~0.05 mode=shrink min_order=2 min_keep=0.2',
        wood: 'sides=7|5|3|3 min_radius=0.002|0.003|0.005|0.007 tile=0.3',
        leafshape: 'outline=ovate length=0.1~0.01 width=0.09~0.008 petiole=0.012 teeth=16 tooth_depth=0.1 fold=0.1 curl=0.1 detail=5|4|3|2 veins=6',
        leaves: 'orders=2 density=8|4|2|1~1 start=0.2 arrangement=distichous angle=60~5 droop=0.1 roll=25 scale=1.5|2.121|3|4.243~0.1 size_jitter=0.2',
        leafmesh: 'color_var=0.1 hue_var=0.05',
        leaftexture: 'size=256 color=#3f6a20 vein_color=#86a650 vein_width=0.03 vein_angle=45 vein_contrast=0.45 network=16 color_var=0.12 roughness=0.6 normal_strength=0.7',
        bark: 'size=512 furrows=2 stretch=3 furrow_width=0.03 furrow_depth=0.02 plates=4 plate_depth=0.02 lenticels=25 lenticel_contrast=0.5 octaves=5 moss=0.1 roughness=0.55 normal_strength=0.6 color_dark=#5a4a3e color_light=#9b8a78 moss_color=#5b6b2c lenticel_color=#c9b9a0',
    }),
};

const DEFAULT_SPECIES = SPECIES_LIBRARY.beech_like;
