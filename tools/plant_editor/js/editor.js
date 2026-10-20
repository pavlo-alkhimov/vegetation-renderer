// Table editor: nodes pinned to grid cells, wires between them, keyboard navigation; inspector for parameters.
//
// Value nodes (Lod, Seed, Const, Expr) connect to Expr inputs or bind to any numeric parameter / seed of a node:
// C on the value node, arrows to the target, Enter, then pick the parameter from the list.
// Keys (table):  arrows move the cursor · Ctrl+arrows move the node · Enter edit node / insert node in an empty cell
//                Del remove · C connect (arrows to target, Enter) · X disconnect inputs · Space preview · Ctrl+D duplicate
// Keys (edit):   Up/Down parameter · Left/Right value (Shift x10) · Alt/Ctrl+Left/Right variation · L per-LOD values
//                Enter cycle enum · Esc back

const CELL_W = 150, CELL_H = 76, CELL_PAD = 7;
const TYPE_COLORS = { value: '#8a8f99', skeleton: '#8a6a45', organ: '#4f8a3a', placements: '#6f9a4a', mesh: '#3f6f9e', texture: '#7a4f9a', output: '#a98a2c' };

function editor_init(A, canvas, inspector)
{
    const E = { A: A, canvas: canvas, ctx: canvas.getContext('2d'), insp: inspector, cur: [0, 0], mode: 'nav',
                ins_sel: 0, wire_src: 0, scroll: [0, 0], row: 0, msg: '', msg_t: 0, drag: null, insp_node: -1 };
    canvas.tabIndex = 0;
    canvas.addEventListener('mousedown', e => {
        canvas.focus();
        const c = editor_cell_at(E, e);
        if (E.mode === 'edit') editor_set_mode(E, 'nav');
        if (E.mode === 'insert' || E.mode === 'wire') { E.mode = 'nav'; }
        E.cur = c;
        const n = node_at(A.g, c[0], c[1]);
        E.drag = n ? { id: n.id } : null;
        editor_select_changed(E);
    });
    canvas.addEventListener('mouseup', e => {
        if (!E.drag) return;
        const c = editor_cell_at(E, e), n = node_find(A.g, E.drag.id);
        E.drag = null;
        if (!n || (c[0] === n.col && c[1] === n.row)) return;
        const free = node_at(A.g, c[0], c[1]) ? nearest_free_cell(A.g, c[0], c[1]) : c;
        n.col = free[0]; n.row = free[1]; E.cur = free.slice();
        A.changed(false);
    });
    canvas.addEventListener('dblclick', e => { E.cur = editor_cell_at(E, e); editor_enter(E); });
    return E;
}

function editor_cell_at(E, e)
{
    const r = E.canvas.getBoundingClientRect();
    return [Math.max(0, Math.floor((e.clientX - r.left + E.scroll[0]) / CELL_W)), Math.max(0, Math.floor((e.clientY - r.top + E.scroll[1]) / CELL_H))];
}

function editor_msg(E, s) { E.msg = s; E.msg_t = performance.now(); }

function editor_set_mode(E, m)
{
    E.mode = m;
    if (m === 'nav') E.canvas.focus();
    inspector_build(E);
}

function editor_select_changed(E)
{
    const n = node_at(E.A.g, E.cur[0], E.cur[1]);
    const id = n ? n.id : 0;
    if (id !== E.insp_node) { E.insp_node = id; E.row = -1; inspector_build(E); }
}

function editor_enter(E)
{
    const n = node_at(E.A.g, E.cur[0], E.cur[1]);
    if (n) { E.row = -1; editor_set_mode(E, 'edit'); }
    else { E.mode = 'insert'; E.ins_sel = Math.max(0, E.ins_sel); }
}

// Returns true if the key was handled.
function editor_key(E, e)
{
    const A = E.A, g = A.g, k = e.key;
    const n = node_at(g, E.cur[0], E.cur[1]);
    const arrows = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
    if (E.mode === 'insert') {
        if (k === 'Escape') E.mode = 'nav';
        else if (k === 'ArrowUp') E.ins_sel = (E.ins_sel + OP_NAMES.length - 1) % OP_NAMES.length;
        else if (k === 'ArrowDown') E.ins_sel = (E.ins_sel + 1) % OP_NAMES.length;
        else if (k === 'Enter') {
            const op = OP_NAMES[E.ins_sel];
            if (op === 'Output' && graph_output(g)) editor_msg(E, 'there is already an Output node');
            else {
                const m = node_add(g, op, E.cur[0], E.cur[1]);
                // Random-using nodes get their seed wired to the channel's Seed node, if there is one (visible, editable).
                if (OPS[op].chan) { const sn = g.nodes.find(q => q.op === 'Inputs'); if (sn) bind(g, REF(sn.id, OPS.Inputs.outs.indexOf(OPS[op].chan)), m.id, 'seed'); } A.changed(true); editor_msg(E, 'added ' + m.name); E.insp_node = -1; editor_select_changed(E); }
            E.mode = 'nav';
        } else if (k.length === 1 && /[a-z]/i.test(k)) {
            const i = OP_NAMES.findIndex(o => o[0].toLowerCase() === k.toLowerCase());
            if (i >= 0) E.ins_sel = i;
        } else return false;
        return true;
    }
    if (E.mode === 'wire') {
        if (arrows[k]) { E.cur = [Math.max(0, E.cur[0] + arrows[k][0]), Math.max(0, E.cur[1] + arrows[k][1])]; editor_scroll_to(E); }
        else if (k === 'Escape') E.mode = 'nav';
        else if (k === 'Enter') {
            const src = node_find(g, E.wire_src);
            if (!n) editor_msg(E, 'no node here');
            else if (OPS[src.op].out === 'value' && pick_slot(g, E.wire_src, n.id) < 0) {
                E.bind_list = bindable_params(n.op);
                if (!E.bind_list.length) editor_msg(E, n.name + ' has no numeric parameters');
                else { E.bind_dst = n.id; E.bind_sel = 0; E.mode = 'bind'; return true; }
            } else {
                const slot = pick_slot(g, E.wire_src, n.id);
                if (slot < 0) editor_msg(E, n.name + ' has no input of type ' + OPS[node_find(g, E.wire_src).op].out);
                else {
                    const err = connect(g, E.wire_src, n.id, slot);
                    if (err) editor_msg(E, err); else { editor_msg(E, 'connected to ' + n.name + ' input ' + (slot + 1)); A.changed(true); }
                }
            }
            E.mode = 'nav';
            editor_select_changed(E);
        } else return false;
        return true;
    }
    if (E.mode === 'port') {
        const outs = OPS[node_find(g, E.port_node).op].outs;
        if (k === 'Escape') E.mode = 'nav';
        else if (k === 'ArrowUp') E.port_sel = (E.port_sel + outs.length - 1) % outs.length;
        else if (k === 'ArrowDown') E.port_sel = (E.port_sel + 1) % outs.length;
        else if (k === 'Enter') { E.wire_src = REF(E.port_node, E.port_sel); E.mode = 'wire'; editor_msg(E, 'connect ' + ref_name(g, E.wire_src) + ': move to the target node, Enter'); }
        else return false;
        return true;
    }
    if (E.mode === 'bind') {
        if (k === 'Escape') E.mode = 'nav';
        else if (k === 'ArrowUp') E.bind_sel = (E.bind_sel + E.bind_list.length - 1) % E.bind_list.length;
        else if (k === 'ArrowDown') E.bind_sel = (E.bind_sel + 1) % E.bind_list.length;
        else if (k === 'Enter') {
            const p = E.bind_list[E.bind_sel], err = bind(g, E.wire_src, E.bind_dst, p);
            if (err) editor_msg(E, err); else { editor_msg(E, node_find(g, E.bind_dst).name + '.' + p + ' <- ' + ref_name(g, E.wire_src)); A.changed(true); }
            E.mode = 'nav'; E.insp_node = -1; editor_select_changed(E);
        } else return false;
        return true;
    }
    if (E.mode === 'edit') return inspector_key(E, e);
    // Navigation mode.
    if (arrows[k]) {
        const d = arrows[k];
        if ((e.ctrlKey || e.metaKey) && n) {
            const c = [Math.max(0, n.col + d[0]), Math.max(0, n.row + d[1])];
            if (!node_at(g, c[0], c[1])) { n.col = c[0]; n.row = c[1]; E.cur = c; A.changed(false); }
        } else E.cur = [Math.max(0, E.cur[0] + d[0]), Math.max(0, E.cur[1] + d[1])];
        editor_scroll_to(E);
        editor_select_changed(E);
        return true;
    }
    if (k === 'Enter') { editor_enter(E); return true; }
    if ((k === 'Delete' || k === 'Backspace') && n) {
        node_remove(g, n.id);
        if (A.preview_id === n.id) A.preview_id = 0;
        editor_msg(E, 'removed ' + n.name); A.changed(true); editor_select_changed(E); return true;
    }
    if ((k === 'c' || k === 'C') && n) {
        if (OPS[n.op].out === 'output') { editor_msg(E, 'Output has no output'); return true; }
        if (OPS[n.op].outs) { E.port_node = n.id; E.port_sel = 0; E.mode = 'port'; return true; }
        E.wire_src = n.id; E.mode = 'wire'; editor_msg(E, 'connect ' + n.name + ': move to the target node, Enter'); return true;
    }
    if ((k === 'x' || k === 'X') && n) { n.ins = n.ins.map(() => 0); n.binds = {}; editor_msg(E, 'disconnected inputs of ' + n.name); A.changed(true); return true; }
    if ((k === 'p' || k === 'P') && n) { A.preview_id = A.preview_id === n.id ? 0 : n.id; A.refit = true; A.changed(false); return true; }
    if ((k === 'd' || k === 'D') && (e.ctrlKey || e.metaKey) && n) {
        const c = nearest_free_cell(g, n.col + 1, n.row);
        const m = node_add(g, n.op, c[0], c[1]);
        m.p = JSON.parse(JSON.stringify(n.p)); m.ins = n.ins.slice(); m.binds = Object.assign({}, n.binds);
        editor_msg(E, 'duplicated as ' + m.name); A.changed(true); return true;
    }
    return false;
}

function editor_scroll_to(E)
{
    const w = E.canvas.clientWidth, h = E.canvas.clientHeight;
    const x = E.cur[0] * CELL_W, y = E.cur[1] * CELL_H;
    if (x < E.scroll[0]) E.scroll[0] = x;
    if (x + CELL_W > E.scroll[0] + w) E.scroll[0] = x + CELL_W - w;
    if (y < E.scroll[1]) E.scroll[1] = y;
    if (y + CELL_H > E.scroll[1] + h) E.scroll[1] = y + CELL_H - h;
}

// y of a node's output port (multi-output nodes: one row of ~26 px per output under the title).
function out_y(n, ref)
{
    const o = OPS[n.op].outs;
    return o ? n.row * CELL_H + CELL_PAD + 32 + ref_port(ref) * 26 : n.row * CELL_H + CELL_H / 2;
}

function port_y(op, i) { const n = OPS[op].ins.length; return CELL_PAD + 22 + (CELL_H - 2 * CELL_PAD - 26) * (n === 1 ? 0.5 : i / (n - 1)); }

function editor_draw(E)
{
    const A = E.A, g = A.g, c = E.canvas, dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth, h = c.clientHeight;
    if (c.width !== Math.floor(w * dpr) || c.height !== Math.floor(h * dpr)) { c.width = Math.floor(w * dpr); c.height = Math.floor(h * dpr); }
    const x = E.ctx;
    x.setTransform(dpr, 0, 0, dpr, 0, 0);
    x.fillStyle = '#1c1d20'; x.fillRect(0, 0, w, h);
    x.save();
    x.translate(-E.scroll[0], -E.scroll[1]);
    // Grid.
    let mc = 0, mr = 0;
    for (const n of g.nodes) { mc = Math.max(mc, n.col); mr = Math.max(mr, n.row); }
    const cols = Math.max(mc + 3, Math.ceil((w + E.scroll[0]) / CELL_W)), rows = Math.max(mr + 3, Math.ceil((h + E.scroll[1]) / CELL_H));
    x.strokeStyle = '#2a2c30'; x.lineWidth = 1;
    for (let i = 0; i <= cols; i++) { x.beginPath(); x.moveTo(i * CELL_W + 0.5, 0); x.lineTo(i * CELL_W + 0.5, rows * CELL_H); x.stroke(); }
    for (let j = 0; j <= rows; j++) { x.beginPath(); x.moveTo(0, j * CELL_H + 0.5); x.lineTo(cols * CELL_W, j * CELL_H + 0.5); x.stroke(); }
    // Wires.
    x.lineWidth = 2;
    for (const n of g.nodes)
        for (let i = 0; i < n.ins.length; i++) {
            const s = node_find(g, n.ins[i]);
            if (!s) continue;
            const x0 = s.col * CELL_W + CELL_W - CELL_PAD, y0 = out_y(s, n.ins[i]);
            const x1 = n.col * CELL_W + CELL_PAD, y1 = n.row * CELL_H + port_y(n.op, i);
            const dx = Math.max(30, Math.abs(x1 - x0) * 0.5);
            x.strokeStyle = TYPE_COLORS[OPS[s.op].out];
            x.beginPath(); x.moveTo(x0, y0); x.bezierCurveTo(x0 + dx, y0, x1 - dx, y1, x1, y1); x.stroke();
        }
    // Bindings: thin dashed lines from value nodes to the top edge of the bound node.
    x.lineWidth = 1; x.setLineDash([3, 3]);
    for (const n of g.nodes)
        for (const k of Object.keys(n.binds)) {
            const s = node_find(g, n.binds[k]);
            if (!s) continue;
            const x0 = s.col * CELL_W + CELL_W - CELL_PAD, y0 = out_y(s, n.binds[k]);
            const x1 = n.col * CELL_W + CELL_W / 2, y1 = n.row * CELL_H + CELL_PAD;
            x.strokeStyle = k === 'seed' ? 'rgba(138,143,153,0.35)' : 'rgba(200,205,215,0.8)';
            x.beginPath(); x.moveTo(x0, y0); x.bezierCurveTo(x0 + 40, y0, x1, y1 - 40, x1, y1); x.stroke();
        }
    x.setLineDash([]);
    // Nodes.
    for (const n of g.nodes) {
        const op = OPS[n.op], bx = n.col * CELL_W + CELL_PAD, by = n.row * CELL_H + CELL_PAD, bw = CELL_W - 2 * CELL_PAD, bh = node_span(n) * CELL_H - 2 * CELL_PAD;
        x.fillStyle = '#2d3036'; x.fillRect(bx, by, bw, bh);
        x.fillStyle = TYPE_COLORS[op.out]; x.fillRect(bx, by, bw, 17);
        x.strokeStyle = n._err ? '#d0503c' : '#44474e'; x.lineWidth = 1; x.strokeRect(bx + 0.5, by + 0.5, bw - 1, bh - 1);
        x.font = 'bold 11px sans-serif'; x.fillStyle = '#f2f2f2'; x.textBaseline = 'middle';
        x.fillText(n.name, bx + 6, by + 9);
        if (A.preview_id === n.id) { x.fillStyle = '#ffd860'; x.fillText('◉', bx + bw - 14, by + 9); }
        x.font = '11px sans-serif'; x.fillStyle = '#b8bcc4';
        if (op.outs) {
            // One labelled row per output, with its current value.
            x.font = '11px monospace';
            for (let o = 0; o < op.outs.length; o++) {
                const y = out_y(n, REF(n.id, o)), v = n._val ? n._val[o] : undefined;
                x.fillStyle = '#b8bcc4'; x.fillText(op.outs[o], bx + 10, y);
                x.fillStyle = '#9ad08a'; x.fillText(v === undefined ? '' : String(Math.round(v * 1000) / 1000), bx + 78, y);
                x.fillStyle = TYPE_COLORS.value; x.beginPath(); x.arc(bx + bw, y, 4, 0, Math.PI * 2); x.fill();
            }
            continue;
        }
        x.fillText(n.op + (op.chan ? '  · ' + op.chan : ''), bx + 10, by + 30);
        x.fillStyle = n._err ? '#e0705c' : n._cached ? '#7f8590' : '#9ad08a';
        const nb = Object.keys(n.binds).filter(k => k !== 'seed').length;
        const st = n._err ? (n._err === 'skipped' ? '' : 'error') : typeof n._val === 'number' ? '= ' + (Math.round(n._val * 1000) / 1000) :
                   n._ms === undefined ? '' : (n._cached ? 'cached' : n._ms.toFixed(1) + ' ms') + (nb ? '  \u00b7 ' + nb + ' bound' : '');
        x.fillText(st, bx + 10, by + bh - 10);
        if (op.out === 'texture' && n._tex) {
            if (!n._tex._thumb) n._tex._thumb = rgba_canvas(n._tex.albedo, n._tex.size);
            const s = Math.min(bw / 3, bh - 20);
            x.drawImage(n._tex._thumb, bx + bw - s - 3, by + 19, s, s);
        }
        for (let i = 0; i < op.ins.length; i++) {
            x.fillStyle = n.ins[i] ? TYPE_COLORS[op.ins[i]] : '#1c1d20';
            x.strokeStyle = TYPE_COLORS[op.ins[i]];
            x.beginPath(); x.arc(bx, n.row * CELL_H + port_y(n.op, i), 4, 0, Math.PI * 2); x.fill(); x.stroke();
        }
        if (op.out !== 'output' && !op.outs) { x.fillStyle = TYPE_COLORS[op.out]; x.beginPath(); x.arc(bx + bw, by + bh / 2, 4, 0, Math.PI * 2); x.fill(); }
    }
    // Cursor.
    x.strokeStyle = E.mode === 'wire' ? '#5fd0ff' : E.mode === 'edit' ? '#ffd860' : '#e8e8e8';
    x.lineWidth = 2;
    x.strokeRect(E.cur[0] * CELL_W + 2, E.cur[1] * CELL_H + 2, CELL_W - 4, CELL_H - 4);
    if (E.mode === 'wire') {
        const s = node_find(g, E.wire_src);
        if (s) {
            x.setLineDash([5, 4]); x.strokeStyle = '#5fd0ff';
            x.beginPath(); x.moveTo(s.col * CELL_W + CELL_W - CELL_PAD, out_y(s, E.wire_src));
            x.lineTo(E.cur[0] * CELL_W + CELL_PAD, E.cur[1] * CELL_H + CELL_H / 2); x.stroke(); x.setLineDash([]);
        }
    }
    if (E.mode === 'port') {
        const s = node_find(g, E.port_node), outs = OPS[s.op].outs, lx = s.col * CELL_W + CELL_W - 20, ly = s.row * CELL_H + 10, lh = 17;
        x.fillStyle = '#25272c'; x.fillRect(lx, ly, 150, outs.length * lh + 26);
        x.strokeStyle = '#5a5e66'; x.strokeRect(lx + 0.5, ly + 0.5, 150, outs.length * lh + 26);
        x.font = '12px sans-serif'; x.fillStyle = '#9a9ea6'; x.fillText('connect output:', lx + 8, ly + 12);
        for (let i = 0; i < outs.length; i++) {
            if (i === E.port_sel) { x.fillStyle = '#3d5f88'; x.fillRect(lx + 2, ly + 22 + i * lh, 146, lh); }
            x.fillStyle = '#eee'; x.fillText(outs[i], lx + 12, ly + 31 + i * lh);
        }
    }
    if (E.mode === 'bind') {
        const d = node_find(g, E.bind_dst), lx = d.col * CELL_W + 10, ly = d.row * CELL_H + 10, lh = 17;
        x.fillStyle = '#25272c'; x.fillRect(lx, ly, 170, E.bind_list.length * lh + 26);
        x.strokeStyle = '#5a5e66'; x.strokeRect(lx + 0.5, ly + 0.5, 170, E.bind_list.length * lh + 26);
        x.font = '12px sans-serif'; x.fillStyle = '#9a9ea6'; x.fillText('bind to parameter:', lx + 8, ly + 12);
        for (let i = 0; i < E.bind_list.length; i++) {
            if (i === E.bind_sel) { x.fillStyle = '#3d5f88'; x.fillRect(lx + 2, ly + 22 + i * lh, 166, lh); }
            x.fillStyle = d.binds[E.bind_list[i]] ? '#ffd860' : '#eee'; x.fillText(E.bind_list[i], lx + 12, ly + 31 + i * lh);
        }
    }
    if (E.mode === 'insert') {
        const lx = E.cur[0] * CELL_W + 10, ly = E.cur[1] * CELL_H + 10, lh = 17;
        x.fillStyle = '#25272c'; x.fillRect(lx, ly, 170, OP_NAMES.length * lh + 8);
        x.strokeStyle = '#5a5e66'; x.strokeRect(lx + 0.5, ly + 0.5, 170, OP_NAMES.length * lh + 8);
        x.font = '12px sans-serif';
        for (let i = 0; i < OP_NAMES.length; i++) {
            if (i === E.ins_sel) { x.fillStyle = '#3d5f88'; x.fillRect(lx + 2, ly + 4 + i * lh, 166, lh); }
            x.fillStyle = TYPE_COLORS[OPS[OP_NAMES[i]].out]; x.fillRect(lx + 6, ly + 9 + i * lh, 7, 7);
            x.fillStyle = '#eee'; x.fillText(OP_NAMES[i], lx + 20, ly + 13 + i * lh);
        }
    }
    x.restore();
    if (E.msg && performance.now() - E.msg_t < 4000) {
        x.font = '12px sans-serif'; x.fillStyle = 'rgba(20,20,22,0.85)';
        const tw = x.measureText(E.msg).width;
        x.fillRect(6, h - 24, tw + 12, 19); x.fillStyle = '#ffd860'; x.fillText(E.msg, 12, h - 14);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Inspector: species parameters of the selected node (value + variation), effective values for the current seeds.
// Numeric rows can hold one value per LOD level ('LOD' toggle, key L): the slider then edits the current level's value.

function pval(p, lod) { return p.lods ? p.lods[lod] : p.v; }
function pset(p, v, lod) { if (p.lods) { p.lods[lod] = v; p.v = p.lods[0]; } else p.v = v; }
function plod_toggle(p) { if (p.lods) { p.v = p.lods[0]; delete p.lods; } else p.lods = [p.v, p.v, p.v, p.v]; }

function inspector_build(E)
{
    const A = E.A, el = E.insp, n = node_find(A.g, E.insp_node);
    el.innerHTML = '';
    el.style.visibility = n ? 'visible' : 'hidden';
    if (!n) return;
    const op = OPS[n.op];
    const head = document.createElement('div');
    head.className = 'insp-head';
    head.innerHTML = '<input class="name" value="' + n.name + '" title="node name (part of the node seed)"> <span class="op">' + n.op + '</span>' +
        '<div class="desc">' + op.desc + '</div>' +
        '<div class="desc">LOD input: <b>' + A.lod + '</b> (per-LOD rows show and edit this level) &nbsp; seed channel: <b>' + (op.chan || '-') + '</b>' + (op.chan ? ' &nbsp; node seed ' + (n._seed !== undefined ? hex32(n._seed) : '-') : '') + '</div>' +
        (n.binds.seed ? '<div class="desc">seed \u2190 <b>' + ref_name(A.g, n.binds.seed) + '</b> <button class="unbind" data-p="seed">\u00d7</button></div>' : '') +
        (n._err && n._err !== 'skipped' ? '<div class="err">' + n._err + '</div>' : '');
    el.appendChild(head);
    head.querySelectorAll('.unbind').forEach(b => b.addEventListener('click', () => { delete n.binds[b.dataset.p]; A.changed(true); }));
    head.querySelector('.name').addEventListener('change', ev => {
        const v = ev.target.value.trim().replace(/[^\w]/g, '_');
        if (!v || (node_by_name(A.g, v) && node_by_name(A.g, v) !== n)) { ev.target.value = n.name; return; }
        n.name = v; A.changed(true);
    });
    // Parameter rows wrap into as many columns as the panel width allows. Numeric values are slider bars: a framed
    // rectangle with the name and value, filled (blue) up to the value; drag inside it (Ctrl: fine), double-click to
    // type a value.
    const tbl = document.createElement('div');
    tbl.className = 'pgrid';
    // Rows grouped by kind (numbers, choices, colours, text; bound parameters stay with their kind), in declaration
    // order inside a group; a rule one column wide separates the groups. E.order is the visual order for Up/Down.
    const kind = d => d.type === 'f' || d.type === 'i' ? 0 : d.type === 'e' ? 1 : d.type === 'c' ? 2 : 3;
    E.order = op.params.map((d, i) => i).sort((a, b) => kind(op.params[a]) - kind(op.params[b]) || a - b);
    if (E.order.indexOf(E.row) < 0) E.row = E.order.length ? E.order[0] : 0;
    let last_kind = -1;
    E.order.forEach(function (i) {
        const d = op.params[i];
        if (last_kind >= 0 && kind(d) !== last_kind) { const sep = document.createElement('div'); sep.className = 'psep'; tbl.appendChild(sep); }
        last_kind = kind(d);
        const p = n.p[d.name], tr = document.createElement('div');
        tr.className = 'prow' + (E.mode === 'edit' && E.row === i ? ' sel' : '');
        tr.dataset.row = i;
        tr.title = d.desc;
        const td_val = document.createElement('div'), td_now = document.createElement('span');
        td_val.className = 'pval'; td_now.className = 'now';
        const label = function () { const l = document.createElement('span'); l.className = 'pname'; l.textContent = d.name; td_val.appendChild(l); };
        if (n.binds[d.name]) {
            label();
            const s = document.createElement('span');
            s.innerHTML = '← <b>' + ref_name(A.g, n.binds[d.name]) + '</b> ';
            td_val.appendChild(s);
            const ub = document.createElement('button');
            ub.textContent = '×'; ub.title = 'unbind (key U)';
            ub.addEventListener('click', () => { delete n.binds[d.name]; A.changed(true); });
            td_val.appendChild(ub);
        } else if (d.type === 's') {
            label();
            const ti = document.createElement('input');
            ti.type = 'text'; ti.value = p.v; ti.className = 'text';
            ti.addEventListener('change', () => { p.v = ti.value; A.changed(false); });
            td_val.appendChild(ti);
        } else if (d.type === 'f' || d.type === 'i') {
            const bar = document.createElement('div'), fill = document.createElement('div'), txt = document.createElement('span');
            bar.className = 'sbar'; fill.className = 'sfill'; txt.className = 'stext';
            bar.appendChild(fill); bar.appendChild(txt);
            const show = function () {
                const v = pval(p, A.lod);
                fill.style.width = (clamp((v - d.min) / (d.max - d.min), 0, 1) * 100) + '%';
                txt.innerHTML = '<span>' + d.name + '</span><b>' + (Math.round(v * 1000) / 1000) + '</b>';
            };
            // 'per LOD' checkbox: off = one value for all LOD levels; on = four values L0..L3 (the field of the current
            // LOD input is outlined; the bar and arrows edit that one).
            const lodvals = document.createElement('div');
            lodvals.className = 'lodvals';
            const set = function (v, lod) {
                v = clamp(parseFloat(v), d.min, d.max); if (isNaN(v)) return;
                if (d.type === 'i') v = Math.round(v); else v = Math.round(v / d.step) * d.step;
                pset(p, Math.round(v * 1e6) / 1e6, lod); show(); fill_lods(); A.changed(false);
            };
            const fill_lods = function () {
                if (!p.lods) { lodvals.innerHTML = ''; return; }
                if (!lodvals.children.length)
                    for (let l = 0; l < 4; l++) {
                        const s = document.createElement('span'); s.textContent = 'L' + l; lodvals.appendChild(s);
                        const li = document.createElement('input'); li.type = 'number'; li.step = d.step; li.className = l === A.lod ? 'cur' : '';
                        li.title = 'value at LOD ' + l; li.addEventListener('change', () => set(li.value, l)); lodvals.appendChild(li);
                    }
                lodvals.querySelectorAll('input').forEach((li, l) => { if (document.activeElement !== li) li.value = p.lods[l]; });
            };
            bar.addEventListener('mousedown', ev => {
                if (ev.button !== 0) return;
                ev.preventDefault(); E.row = i;
                if (ev.detail >= 2) return;           // second click of a double-click: typing, not sliding
                const r = bar.getBoundingClientRect(), v0 = pval(p, A.lod), x0 = ev.clientX;
                bar._prev = v0;
                const at = e2 => e2.ctrlKey ? v0 + (e2.clientX - x0) / r.width * (d.max - d.min) * 0.05
                                           : d.min + clamp((e2.clientX - r.left) / r.width, 0, 1) * (d.max - d.min);
                if (!ev.ctrlKey) set(at(ev), A.lod);
                bar.classList.add('drag');
                const move = e2 => set(at(e2), A.lod);
                const up = () => { bar.classList.remove('drag'); window.removeEventListener('mousemove', move); window.removeEventListener('mouseup', up); };
                window.addEventListener('mousemove', move); window.addEventListener('mouseup', up);
            });
            bar.addEventListener('dblclick', () => {
                if (bar._prev !== undefined) set(bar._prev, A.lod);    // undo the jump of the first click
                const ni = document.createElement('input');
                ni.type = 'number'; ni.step = d.step; ni.value = pval(p, A.lod); ni.className = 'sedit';
                bar.appendChild(ni); ni.focus(); ni.select();
                let closed = false;
                const done = ok => { if (closed) return; closed = true; if (ok) set(ni.value, A.lod); ni.remove(); };
                ni.addEventListener('keydown', e2 => { if (e2.key === 'Enter') done(true); else if (e2.key === 'Escape') done(false); e2.stopPropagation(); });
                ni.addEventListener('blur', () => done(true));
            });
            show();
            const lb = document.createElement('label');
            lb.className = 'perlod'; lb.title = 'separate value for each LOD level (key L)';
            lb.innerHTML = '<input type="checkbox"' + (p.lods ? ' checked' : '') + '>per LOD';
            lb.querySelector('input').addEventListener('change', () => { plod_toggle(p); A.changed(false); inspector_build(E); });
            const vr = document.createElement('input');
            vr.type = 'number'; vr.min = 0; vr.step = d.step; vr.value = p.vari; vr.className = 'vari'; vr.title = 'variation by seed (±)';
            vr.addEventListener('change', () => { p.vari = Math.max(0, parseFloat(vr.value) || 0); A.changed(false); });
            const pm = document.createElement('span'); pm.className = 'pm'; pm.textContent = '±';
            td_val.appendChild(bar); td_val.appendChild(pm); td_val.appendChild(vr); td_val.appendChild(lb);
            tr.appendChild(td_val); tr.appendChild(td_now); tr.appendChild(lodvals);
            fill_lods();
        } else if (d.type === 'e') {
            label();
            const s = document.createElement('select');
            d.labels.forEach((l, k) => { const o = document.createElement('option'); o.value = k; o.textContent = l; s.appendChild(o); });
            s.value = p.v;
            s.addEventListener('change', () => { p.v = parseInt(s.value, 10); A.changed(false); });
            td_val.appendChild(s);
        } else {
            label();
            const ci = document.createElement('input');
            ci.type = 'color'; ci.value = p.v;
            ci.addEventListener('input', () => { p.v = ci.value; A.changed(false); });
            td_val.appendChild(ci);
        }
        if (!td_val.parentNode) { tr.appendChild(td_val); tr.appendChild(td_now); }
        tr.addEventListener('mousedown', () => { E.row = i; });
        tbl.appendChild(tr);
    });
    el.appendChild(tbl);
    if (n.op === 'Inputs') {
        // The instance values themselves (not part of the species): the three seeds and the LOD level.
        const inst = document.createElement('div');
        inst.className = 'instance';
        inst.innerHTML = '<div class="desc">Instance inputs (not stored in the species; a level stores only the seeds):</div><div id="seeds"></div><div id="seed_list"></div>';
        el.appendChild(inst);
        seeds_ui_build();
        seeds_ui_refresh();
    }
    const ins = OPS[n.op].ins;
    if (ins.length) {
        const d = document.createElement('div');
        d.className = 'desc';
        d.innerHTML = 'inputs: ' + ins.map((t, i) => { return t + ' ← ' + (n.ins[i] ? '<b>' + ref_name(A.g, n.ins[i]) + '</b>' : '<i>none</i>'); }).join(', ');
        el.appendChild(d);
    }
    inspector_values(E);
}

// Effective values (after variation by the current seeds), refreshed after every evaluation.
function inspector_values(E)
{
    const n = node_find(E.A.g, E.insp_node);
    if (!n || !n._P) return;
    const op = OPS[n.op];
    const rows = E.insp.querySelectorAll('[data-row]');
    rows.forEach(tr => {
        const d = op.params[parseInt(tr.dataset.row, 10)], v = n._P[d.name];
        const td = tr.querySelector('.now');
        if (d.type === 'e') td.textContent = d.name === 'size' ? v : d.labels[v];
        else if (d.type === 's') td.textContent = typeof n._val === 'number' ? '= ' + (Math.round(n._val * 1000) / 1000) : '';
        else if (d.type === 'c') td.textContent = '';
        else td.textContent = d.type === 'i' ? String(v) : (Math.round(v * 1000) / 1000).toString();
    });
}

function lod_html(p)
{
    if (!p.lods) return '';
    return p.lods.map((v, i) => i === A.lod ? '<b>' + (Math.round(v * 1000) / 1000) + '</b>' : String(Math.round(v * 1000) / 1000)).join(' | ');
}

function inspector_key(E, e)
{
    const A = E.A, n = node_find(A.g, E.insp_node);
    if (!n) { editor_set_mode(E, 'nav'); return true; }
    const op = OPS[n.op], k = e.key;
    if (k === 'Escape') { editor_set_mode(E, 'nav'); return true; }
    if (!op.params.length) return k === 'Enter';
    const d = op.params[E.row], p = n.p[d.name];
    const sel = function () {
        E.insp.querySelectorAll('[data-row]').forEach(tr => {
            const on = parseInt(tr.dataset.row, 10) === E.row;
            tr.classList.toggle('sel', on);
            if (on) tr.scrollIntoView({ block: 'nearest', inline: 'nearest' });     // rows in columns scrolled out of view
        });
    };
    const oi = Math.max(0, E.order.indexOf(E.row)), on = E.order.length;
    if (k === 'ArrowUp') { E.row = E.order[(oi + on - 1) % on]; sel(); return true; }
    if (k === 'ArrowDown') { E.row = E.order[(oi + 1) % on]; sel(); return true; }
    if (k === 'ArrowLeft' || k === 'ArrowRight') {
        const dir = k === 'ArrowLeft' ? -1 : 1, mul = e.shiftKey ? 10 : 1;
        if (d.type === 'e') p.v = (p.v + dir + d.labels.length) % d.labels.length;
        else if (d.type === 'f' || d.type === 'i') {
            if (e.altKey || e.ctrlKey || e.metaKey) p.vari = Math.max(0, Math.round((p.vari + dir * d.step * mul) * 1e5) / 1e5);
            else pset(p, clamp(Math.round((pval(p, A.lod) + dir * d.step * mul) * 1e5) / 1e5, d.min, d.max), A.lod);
        } else return true;
        A.changed(false);
        inspector_build(E);
        return true;
    }
    if ((k === 'u' || k === 'U') && n.binds[d.name]) { delete n.binds[d.name]; A.changed(true); return true; }
    if (n.binds[d.name]) return k === 'ArrowLeft' || k === 'ArrowRight';
    if ((k === 'l' || k === 'L') && (d.type === 'f' || d.type === 'i')) { plod_toggle(p); A.changed(false); inspector_build(E); return true; }
    if (k === 'Enter' && d.type === 'e') { p.v = (p.v + 1) % d.labels.length; A.changed(false); inspector_build(E); return true; }
    if (k === 'Enter' && d.type === 'c') { const ci = E.insp.querySelector('[data-row="' + E.row + '"]').querySelector('input[type=color]'); if (ci) ci.click(); return true; }
    return false;
}
