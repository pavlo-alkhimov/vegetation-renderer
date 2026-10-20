// .species files (the species = node graph + manually defined parameters) and .seeds files (instances).
//
// .species, one node per line (docs/15 recipe format):
//   name = Op @col,row in=a,b,- key=value key=value~variation ...
// '-' = unconnected input; variation after '~' (omitted when 0); enums by label; colours as #rrggbb; '#' comments.
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
            if (d.type === 'e') v = d.labels[p.v];
            else if (d.type === 'c') v = p.v;
            else v = fmt_num(p.v) + (p.vari > 0 ? '~' + fmt_num(p.vari) : '');
            s += ' ' + d.name + '=' + v;
        }
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
        for (const tok of m[3].split(/\s+/).filter(x => x)) {
            if (tok[0] === '@') {
                const c = tok.slice(1).split(',');
                n.col = parseInt(c[0], 10) || 0; n.row = parseInt(c[1], 10) || 0;
                continue;
            }
            const eq = tok.indexOf('=');
            if (eq < 0) { errors.push('line ' + (ln + 1) + ': bad token ' + tok); continue; }
            const key = tok.slice(0, eq), val = tok.slice(eq + 1);
            if (key === 'in') { pending.push({ node: n, refs: val.split(','), ln: ln + 1 }); continue; }
            const d = op.params.find(x => x.name === key);
            if (!d) { errors.push('line ' + (ln + 1) + ': ' + opn + ' has no parameter ' + key); continue; }
            if (d.type === 'e') {
                const k = d.labels.indexOf(val);
                if (k < 0) errors.push('line ' + (ln + 1) + ': ' + key + ' must be one of ' + d.labels.join('|'));
                else n.p[key].v = k;
            } else if (d.type === 'c') {
                if (/^#[0-9a-fA-F]{6}$/.test(val)) n.p[key].v = val.toLowerCase();
                else errors.push('line ' + (ln + 1) + ': ' + key + ' must be #rrggbb');
            } else {
                const parts = val.split('~');
                const v = parseFloat(parts[0]), vari = parts.length > 1 ? parseFloat(parts[1]) : 0;
                if (isNaN(v) || isNaN(vari)) { errors.push('line ' + (ln + 1) + ': bad number ' + val); continue; }
                if (v < d.min || v > d.max) errors.push('line ' + (ln + 1) + ': ' + key + ' outside ' + d.min + '..' + d.max + ' (clamped)');
                n.p[key] = { v: clamp(v, d.min, d.max), vari: Math.max(0, vari) };
            }
        }
    }
    for (const p of pending) {
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

const DEFAULT_SPECIES = [
    '# beech_like - plant editor species (graph + parameters; seeds are separate)',
    'trunk = Trunk @0,0 height=24~2 radius=0.33~0.04 taper=1.1~0.1 flare=0.5~0.15 lean=3~2 wobble=0.025~0.01 segments=12',
    'branches1 = Branches @1,0 in=trunk count=18~2 start=0.3~0.03 end=0.97 phyllotaxis=spiral whorl=4 angle=62~5 angle_jitter=10 az_jitter=25 length=0.55~0.05 length_top=0.3~0.05 radius=0.5 curvature=0.25~0.08 droop=0.06~0.03 wobble=0.06 segments=7',
    'branches2 = Branches @2,0 in=branches1 count=11~1.5 start=0.12 end=0.95 phyllotaxis=distichous whorl=4 angle=52~5 angle_jitter=12 az_jitter=35 length=0.5~0.05 length_top=0.5~0.05 radius=0.55 curvature=0.12~0.05 droop=0.18~0.05 wobble=0.07 segments=5',
    'branches3 = Branches @3,0 in=branches2 count=8~1 start=0.1 end=0.95 phyllotaxis=distichous whorl=4 angle=42~4 angle_jitter=12 az_jitter=40 length=0.42~0.04 length_top=0.6 radius=0.6 curvature=0.05 droop=0.1~0.05 wobble=0.08 segments=3',
    'tropism = Tropism @4,0 in=branches3 gravity=0.08~0.02 photo=0.03~0.01 order_scale=0.6',
    'prune = Prune @5,0 in=tropism shape=ellipsoid crown_base=0.28~0.04 width=0.4~0.04 mode=shrink min_order=1 min_keep=0.2',
    'wood = Wood @6,0 in=prune sides=10 min_radius=0.003 tile=0.6',
    'merge = Merge @7,0 in=wood,leafmesh,-',
    'output = Output @8,0 in=merge,barktexture,leaftexture',
    'leafshape = LeafShape @4,1 outline=ovate length=0.09~0.008 width=0.055~0.005 petiole=0.01 teeth=7 tooth_depth=0.06 fold=0.25~0.05 curl=0.15~0.05 detail=4 veins=7',
    'leaves = Leaves @5,1 in=prune,leafshape orders=2 density=9~1.5 start=0.3 arrangement=distichous angle=55~5 droop=0.15~0.05 roll=25 scale=2.2~0.1 size_jitter=0.2',
    'leafmesh = LeafMesh @6,1 in=leaves,leafshape color_var=0.12 hue_var=0.05',
    'leaftexture = LeafTexture @6,2 in=leafshape size=256 color=#3f6b1e vein_color=#93b055 vein_width=0.035 vein_angle=45~3 vein_contrast=0.45~0.05 network=18 color_var=0.1 roughness=0.55 normal_strength=0.6',
    'barktexture = BarkTexture @7,2 size=512 furrows=6~1 stretch=3 furrow_width=0.08~0.02 furrow_depth=0.12~0.03 plates=10~2 plate_depth=0.08 lenticels=18 lenticel_contrast=0.35 octaves=5 moss=0.15~0.05 roughness=0.8 normal_strength=1 color_dark=#5f605b color_light=#a4a59e moss_color=#5b6b2c',
].join('\n') + '\n';
