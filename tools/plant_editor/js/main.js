// Plant editor: wiring of graph, editor, inspector, seeds panel, viewport, variants, files and export.

const SEED_CHANNELS = ['structure', 'foliage', 'texture'];
const STORE_SPECIES = 'plant_editor.species', STORE_SEEDS = 'plant_editor.seeds', STORE_TITLE = 'plant_editor.title';

const A = {
    g: null, title: 'beech_like', seeds: { structure: 1, foliage: 1, texture: 1 }, lod: 0, locks: {}, seed_list: [],
    cache: new Map(), vcache: new Map(), preview_id: 0, dirty: true, refit: true, structural: false,
    out: null, last_change: 0, variants: null, eval_ms: 0,
};

function $(id) { return document.getElementById(id); }

A.changed = function (structural)
{
    A.dirty = true;
    A.last_change = performance.now();
    if (structural) inspector_build(A.E);
    A.variants = null;
    save_local();
};

function save_local()
{
    try {
        localStorage.setItem(STORE_SPECIES, species_write(A.g, A.title));
        localStorage.setItem(STORE_SEEDS, JSON.stringify({ seeds: A.seeds, locks: A.locks, list: A.seed_list, lod: A.lod }));
        localStorage.setItem(STORE_TITLE, A.title);
    } catch (e) { /* storage unavailable (private mode): ignore */ }
}

function load_species_text(text, title)
{
    const r = species_read(text);
    if (r.errors.length) alert('Species loaded with problems:\n' + r.errors.slice(0, 20).join('\n'));
    A.g = r.graph;
    A.title = title || A.title;
    $('title').value = A.title;
    A.cache.clear();
    A.preview_id = 0;
    A.refit = true;
    if (A.E) { A.E.insp_node = -1; editor_select_changed(A.E); }
    A.changed(true);
}

// ---------------------------------------------------------------------------------------------------------------
// Evaluation and preview

function mesh_bounds(m)
{
    let h = 0, r = 0;
    for (const p of m.parts)
        for (let i = 0; i < p.pos.length; i += 3) { h = Math.max(h, p.pos[i + 1]); r = Math.max(r, Math.hypot(p.pos[i], p.pos[i + 2])); }
    return { h: h, r: r };
}

function mesh_hash(m)
{
    let h = 0x811c9dc5;
    for (const p of m.parts) { h = hash_bytes(new Uint8Array(new Float32Array(p.pos).buffer), h); h = hash_bytes(new Uint8Array(new Uint32Array(p.idx).buffer), h); }
    return h;
}

function tex_hash(t)
{
    if (!t) return 0;
    if (t._hash === undefined) t._hash = hash_bytes(t.normal, hash_bytes(t.albedo, hash_bytes(t.mr)));
    return t._hash;
}

function evaluate()
{
    const g = A.g, V = A.V, t0 = performance.now();
    const ctx = eval_ctx(A.seeds, A.cache, A.lod);
    const outn = graph_output(g);
    const ro = outn ? eval_node(g, outn.id, ctx) : null;
    A.out = ro && !ro.err ? ro.val : null;
    let pn = node_find(g, A.preview_id) || outn;
    let rp = pn ? eval_node(g, pn.id, ctx) : null;
    for (const n of g.nodes) if (!ctx.memo.has(n.id)) eval_node(g, n.id, ctx);     // unconnected nodes still show status
    cache_trim(A.cache, ctx.used);
    A.eval_ms = performance.now() - t0;

    const bark = A.out ? A.out.bark : null, leaf = A.out ? A.out.leaf : null;
    V.tex2d = null;
    let fit = null;
    const type = pn ? OPS[pn.op].out : null;
    if (!rp || rp.err || !rp.val) { view_set_mesh(V, null); }
    else if (type === 'skeleton') { view_set_lines(V, skeleton_lines(rp.val)); fit = { h: rp.val.height, r: rp.val.height * 0.3 }; }
    else if (type === 'placements') { view_set_lines(V, placement_lines(rp.val)); fit = { h: 0, r: 0 }; }
    else if (type === 'organ') {
        const one = mesh_leaves([{ p: [0, 0.02, 0], axis: [1, 0, 0], side: [0, 0, -1], nrm: [0, 1, 0], s: 1, tint: 0 }], rp.val, { color_var: 0, hue_var: 0 });
        view_set_mesh(V, one); fit = { h: rp.val.length * 0.5, r: rp.val.length * 0.6, organ: true };
    }
    else if (type === 'texture') { view_set_mesh(V, null); V.tex2d = rp.val; }
    else if (type === 'mesh') { view_set_mesh(V, rp.val); fit = mesh_bounds(rp.val); }
    else if (type === 'output') { view_set_mesh(V, rp.val.mesh); fit = mesh_bounds(rp.val.mesh); }
    if (fit) {
        if (type === 'placements') { fit = null; }
        else if (A.refit || V.fit_height <= 0 || Math.abs(fit.h - V.fit_height) > 0.3 * V.fit_height) { view_fit(V, fit.h, fit.r); if (fit.organ) V.target = [fit.r * 0.6, 0, 0]; }
    }
    A.refit = false;

    // Status bar.
    let s = 'LOD ' + A.lod + ' \u00b7 eval ' + A.eval_ms.toFixed(1) + ' ms';
    if (A.out && A.out.mesh) {
        const b = mesh_bounds(A.out.mesh);
        let leaves = 0;
        for (const p of A.out.mesh.parts) if (p.mat === 'leaves') leaves = p.idx.length / 3;
        s += ' · ' + mesh_triangles(A.out.mesh).toLocaleString() + ' triangles · height ' + b.h.toFixed(1) + ' m · crown radius ' + b.r.toFixed(1) +
             ' m · mesh ' + hex32(mesh_hash(A.out.mesh)) + ' · textures ' + hex32(hash2(tex_hash(A.out.bark), tex_hash(A.out.leaf)));
    } else s += ' · output incomplete' + (ro && ro.err ? ': ' + ro.err : '');
    if (pn && pn !== outn) s += ' · previewing ' + pn.name + ' (Space on it to return)';
    $('status').textContent = s;
    inspector_values(A.E);
    seeds_ui_refresh();
}

// ---------------------------------------------------------------------------------------------------------------
// Variants: the current species with the next 8 seed combinations (unlocked channels advance), rendered as
// thumbnails one per frame while idle. Texture nodes are skipped; thumbnails use the current textures.

function variant_seeds(k)
{
    const s = {};
    for (const c of SEED_CHANNELS) s[c] = A.locks[c] ? A.seeds[c] : (A.seeds[c] + k + 1) >>> 0;
    return s;
}

function variants_step()
{
    if (!A.variants) {
        if (performance.now() - A.last_change < 350) return;
        A.variants = { k: 0 };
        A.vcache.clear();
    }
    if (A.variants.k >= 8) return;
    const k = A.variants.k++, outn = graph_output(A.g);
    const cv = $('variants').children[k];
    const ctx2 = cv.getContext('2d');
    if (!outn) { ctx2.clearRect(0, 0, cv.width, cv.height); return; }
    const ctx = eval_ctx(variant_seeds(k), A.vcache, A.lod);
    ctx.no_textures = true;
    const n_saved = A.g.nodes.map(n => [n._err, n._ms, n._cached, n._P]);
    const r = eval_node(A.g, outn.id, ctx);
    A.g.nodes.forEach((n, i) => { n._err = n_saved[i][0]; n._ms = n_saved[i][1]; n._cached = n_saved[i][2]; n._P = n_saved[i][3]; });
    if (r.err || !r.val || !r.val.mesh) { ctx2.fillStyle = '#300'; ctx2.fillRect(0, 0, cv.width, cv.height); return; }
    const b = mesh_bounds(r.val.mesh);
    const img = view_thumbnail(A.V, r.val.mesh, cv.width, A.out ? A.out.bark : null, A.out ? A.out.leaf : null, b.h, b.r);
    ctx2.putImageData(img, 0, 0);
    cv.title = 'seeds ' + SEED_CHANNELS.map(c => variant_seeds(k)[c]).join(' ') + ' (click to use)';
}

// ---------------------------------------------------------------------------------------------------------------
// Seeds panel

function seeds_ui_build()
{
    const el = $('seeds');
    el.innerHTML = '';
    for (const c of SEED_CHANNELS) {
        const row = document.createElement('div');
        row.className = 'seed-row';
        row.innerHTML = '<label>' + c + '</label><button data-d="-1">‹</button><input type="number" min="0" step="1"><button data-d="1">›</button>' +
                        '<button data-r="1" title="random">⟳</button><label class="lock"><input type="checkbox"> lock</label>';
        const num = row.querySelector('input[type=number]'), lock = row.querySelector('input[type=checkbox]');
        num.addEventListener('change', () => { A.seeds[c] = (parseInt(num.value, 10) || 0) >>> 0; A.changed(false); });
        row.querySelectorAll('button[data-d]').forEach(b => b.addEventListener('click', () => { A.seeds[c] = (A.seeds[c] + parseInt(b.dataset.d, 10)) >>> 0; A.changed(false); }));
        row.querySelector('button[data-r]').addEventListener('click', () => { A.seeds[c] = (Math.random() * 4294967296) >>> 0; A.changed(false); });
        lock.addEventListener('change', () => { A.locks[c] = lock.checked; A.changed(false); });
        row.dataset.c = c;
        el.appendChild(row);
    }
    const lodrow = document.createElement('div');
    lodrow.className = 'seed-row';
    lodrow.innerHTML = '<label>LOD</label><button data-d="-1">\u2039</button><input type="number" min="0" max="3" step="1" id="lod"><button data-d="1">\u203a</button><span class="hint">network input (0 = full detail)</span>';
    const set_lod = function (v) { A.lod = clamp(v | 0, 0, 3); A.refit = false; A.changed(false); inspector_build(A.E); };
    lodrow.querySelector('#lod').addEventListener('change', e => set_lod(parseInt(e.target.value, 10) || 0));
    lodrow.querySelectorAll('button').forEach(b => b.addEventListener('click', () => set_lod(A.lod + parseInt(b.dataset.d, 10))));
    el.appendChild(lodrow);
    const tools = document.createElement('div');
    tools.className = 'seed-row';
    tools.innerHTML = '<button id="all_next">all ›</button><button id="all_rand">all ⟳</button><button id="keep">+ keep in list</button>';
    el.appendChild(tools);
    $('all_next').addEventListener('click', () => { for (const c of SEED_CHANNELS) if (!A.locks[c]) A.seeds[c] = (A.seeds[c] + 1) >>> 0; A.changed(false); });
    $('all_rand').addEventListener('click', () => { for (const c of SEED_CHANNELS) if (!A.locks[c]) A.seeds[c] = (Math.random() * 4294967296) >>> 0; A.changed(false); });
    $('keep').addEventListener('click', () => { A.seed_list.push(Object.assign({}, A.seeds)); seed_list_ui(); save_local(); });
    seed_list_ui();
}

function seeds_ui_refresh()
{
    const li = $('lod');
    if (li && document.activeElement !== li) li.value = A.lod;
    $('seeds').querySelectorAll('.seed-row[data-c]').forEach(row => {
        const c = row.dataset.c, num = row.querySelector('input[type=number]');
        if (document.activeElement !== num) num.value = A.seeds[c];
        row.querySelector('input[type=checkbox]').checked = !!A.locks[c];
    });
}

function seed_list_ui()
{
    const el = $('seed_list');
    el.innerHTML = A.seed_list.length ? '' : '<span class="hint">kept seed vectors (the instance data of a level) appear here</span>';
    A.seed_list.forEach((s, i) => {
        const b = document.createElement('span');
        b.className = 'chip';
        b.textContent = s.structure + ' ' + s.foliage + ' ' + s.texture;
        b.title = 'click: use · right-click: remove';
        b.addEventListener('click', () => { A.seeds = Object.assign({}, s); A.changed(false); });
        b.addEventListener('contextmenu', e => { e.preventDefault(); A.seed_list.splice(i, 1); seed_list_ui(); save_local(); });
        el.appendChild(b);
    });
}

// ---------------------------------------------------------------------------------------------------------------
// Files

function pick_file(accept, cb)
{
    const inp = document.createElement('input');
    inp.type = 'file'; inp.accept = accept;
    inp.addEventListener('change', () => { const f = inp.files[0]; if (f) f.text().then(t => cb(t, f.name)); });
    inp.click();
}

async function export_glb()
{
    if (!A.out || !A.out.mesh) { alert('The Output node has no result.'); return; }
    const text = species_write(A.g, A.title);
    // All LOD levels 0-3 into one file (cache keys include the level; textures are shared).
    const outn = graph_output(A.g), outs = [];
    for (let lod = 0; lod < 4; lod++) {
        const r = eval_node(A.g, outn.id, eval_ctx(A.seeds, A.cache, lod));
        if (r.err || !r.val || !r.val.mesh) { alert('LOD ' + lod + ': ' + (r.err || 'no result')); A.dirty = true; return; }
        outs.push(r.val);
    }
    A.dirty = true;     // restore node status of the current LOD
    const tris = outs.map(o => mesh_triangles(o.mesh));
    const extras = { species: A.title, species_hash: hex32(hash_str(text)), seeds: Object.assign({}, A.seeds), lods: 4, lod_triangles: tris, generator: 'plant_editor v1' };
    const glb = await gltf_export(outs, extras);
    download(A.title + '_' + A.seeds.structure + '_' + A.seeds.foliage + '_' + A.seeds.texture + '.glb', glb, 'model/gltf-binary');
}

function ui_init()
{
    $('btn_new').addEventListener('click', () => {
        if (!confirm('Start a new species (only an Output node)?')) return;
        A.g = graph_new(); node_add(A.g, 'Output', 3, 0); A.title = 'new_species'; $('title').value = A.title; A.cache.clear(); A.changed(true);
    });
    const lib = $('library');
    for (const name of Object.keys(SPECIES_LIBRARY)) { const o = document.createElement('option'); o.value = name; o.textContent = name; lib.appendChild(o); }
    lib.addEventListener('change', () => {
        const name = lib.value;
        lib.value = '';
        if (name && confirm('Replace the graph with the built-in species ' + name + '?')) load_species_text(SPECIES_LIBRARY[name], name);
        A.E.canvas.focus();
    });
    $('btn_load').addEventListener('click', () => pick_file('.species,.txt', (t, name) => load_species_text(t, name.replace(/\.[^.]*$/, ''))));
    $('btn_save').addEventListener('click', () => download(A.title + '.species', species_write(A.g, A.title), 'text/plain'));
    $('btn_load_seeds').addEventListener('click', () => pick_file('.seeds,.txt', t => { A.seed_list = seeds_read(t); if (A.seed_list.length) A.seeds = Object.assign({}, A.seed_list[0]); seed_list_ui(); A.changed(false); }));
    $('btn_save_seeds').addEventListener('click', () => download(A.title + '.seeds', seeds_write(A.seed_list.length ? A.seed_list : [A.seeds]), 'text/plain'));
    $('btn_glb').addEventListener('click', export_glb);
    $('title').addEventListener('change', e => { A.title = e.target.value.trim().replace(/[^\w-]/g, '_') || 'species'; e.target.value = A.title; save_local(); });
    $('view_mode').addEventListener('change', e => { A.V.mode = parseInt(e.target.value, 10); });
    $('btn_frame').addEventListener('click', () => { A.refit = true; A.dirty = true; });
    $('btn_help').addEventListener('click', () => { $('help').classList.toggle('hidden'); });
    const vs = $('variants');
    for (let k = 0; k < 8; k++) {
        const c = document.createElement('canvas');
        c.width = c.height = 112;
        c.addEventListener('click', () => { A.seeds = variant_seeds(k); A.changed(false); });
        vs.appendChild(c);
    }
    document.addEventListener('keydown', e => {
        const ae = document.activeElement, tag = ae ? ae.tagName : '';
        const text_input = (tag === 'INPUT' && ['range', 'checkbox', 'color', 'button'].indexOf(ae.type) < 0) || tag === 'SELECT' || tag === 'TEXTAREA';
        if (text_input) {
            if (e.key === 'Escape') { ae.blur(); if (A.E.mode === 'edit') editor_set_mode(A.E, 'nav'); e.preventDefault(); }
            return;
        }
        if ((e.key === 'f' || e.key === 'F') && A.E.mode === 'nav' && !e.ctrlKey && !e.metaKey) { A.refit = true; A.dirty = true; e.preventDefault(); return; }
        if (editor_key(A.E, e)) e.preventDefault();
    });
    $('help_text').textContent = help_text();
}

function help_text()
{
    let s = 'TABLE  arrows: move cursor   Ctrl+arrows: move node   Enter: edit node / insert node (empty cell)\n' +
            '       Del: remove   C: connect (arrows to target node, Enter)   X: disconnect inputs   Space: preview node\n' +
            '       Ctrl+D: duplicate   F: frame view   mouse: click selects, drag moves a node, double-click = Enter\n' +
            'EDIT   Up/Down: parameter   Left/Right: value (Shift x10)   Alt+Left/Right: variation   L: per-LOD values   Enter: next enum   Esc: back\n' +
            'VIEW   left drag: orbit   right drag: pan   wheel: zoom\n\n' +
            'Species = graph + parameter values + variations (saved once: .species).  Instance = seeds (structure, foliage,\n' +
            'texture) - the only per-plant data a level needs.  Effective value = value + variation x random(seed) in [-1, 1).\n\nOPERATORS\n';
    for (const name of OP_NAMES) {
        const op = OPS[name];
        s += '\n' + name + '  (' + (op.ins.length ? op.ins.join(', ') : '-') + ' -> ' + op.out + (op.chan ? ', seed: ' + op.chan : '') + ')\n  ' + op.desc + '\n';
        for (const d of op.params) s += '    ' + (d.name + '            ').slice(0, 18) + (d.type === 'e' ? d.labels.join('|') : d.type === 'c' ? 'colour' : d.min + '..' + d.max) + '  ' + d.desc + '\n';
    }
    return s;
}

function main()
{
    A.V = view_init($('view'));
    let text = null, seeds = null;
    try { text = localStorage.getItem(STORE_SPECIES); seeds = JSON.parse(localStorage.getItem(STORE_SEEDS) || 'null'); A.title = localStorage.getItem(STORE_TITLE) || A.title; } catch (e) { /* ignore */ }
    const r = species_read(text || DEFAULT_SPECIES);
    A.g = r.graph.nodes.length ? r.graph : species_read(DEFAULT_SPECIES).graph;
    if (seeds) { A.seeds = seeds.seeds || A.seeds; A.locks = seeds.locks || {}; A.seed_list = seeds.list || []; A.lod = seeds.lod || 0; }
    A.E = editor_init(A, $('graph'), $('inspector'));
    $('title').value = A.title;
    ui_init();
    seeds_ui_build();
    const outn = graph_output(A.g);
    if (outn) A.E.cur = [outn.col, outn.row];
    editor_select_changed(A.E);
    A.E.canvas.focus();
    const frame = function () {
        if (A.dirty) { A.dirty = false; evaluate(); }
        else variants_step();
        editor_draw(A.E);
        view_draw(A.V, A.out ? A.out.bark : null, A.out ? A.out.leaf : null);
        requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
}

window.addEventListener('load', main);
