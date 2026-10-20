// Table editor: nodes pinned to grid cells, wires between them, keyboard navigation; inspector for parameters.
//
// Keys (table):  arrows move the cursor · Ctrl+arrows move the node · Enter edit node / insert node in an empty cell
//                Del remove · C connect (arrows to target, Enter) · X disconnect inputs · Space preview · Ctrl+D duplicate
// Keys (edit):   Up/Down parameter · Left/Right value (Shift x10) · Alt/Ctrl+Left/Right variation · Enter cycle enum · Esc back

const CELL_W = 150, CELL_H = 76, CELL_PAD = 7;
const TYPE_COLORS = { skeleton: '#8a6a45', organ: '#4f8a3a', placements: '#6f9a4a', mesh: '#3f6f9e', texture: '#7a4f9a', output: '#a98a2c' };

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
    if (id !== E.insp_node) { E.insp_node = id; E.row = 0; inspector_build(E); }
}

function editor_enter(E)
{
    const n = node_at(E.A.g, E.cur[0], E.cur[1]);
    if (n) { E.row = 0; editor_set_mode(E, 'edit'); }
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
            else { const m = node_add(g, op, E.cur[0], E.cur[1]); A.changed(true); editor_msg(E, 'added ' + m.name); E.insp_node = -1; editor_select_changed(E); }
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
            if (!n) editor_msg(E, 'no node here');
            else {
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
        E.wire_src = n.id; E.mode = 'wire'; editor_msg(E, 'connect ' + n.name + ': move to the target node, Enter'); return true;
    }
    if ((k === 'x' || k === 'X') && n) { n.ins = n.ins.map(() => 0); editor_msg(E, 'disconnected inputs of ' + n.name); A.changed(true); return true; }
    if (k === ' ' && n) { A.preview_id = A.preview_id === n.id ? 0 : n.id; A.refit = true; A.changed(false); return true; }
    if ((k === 'd' || k === 'D') && (e.ctrlKey || e.metaKey) && n) {
        const c = nearest_free_cell(g, n.col + 1, n.row);
        const m = node_add(g, n.op, c[0], c[1]);
        m.p = JSON.parse(JSON.stringify(n.p)); m.ins = n.ins.slice();
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
            const x0 = s.col * CELL_W + CELL_W - CELL_PAD, y0 = s.row * CELL_H + CELL_H / 2;
            const x1 = n.col * CELL_W + CELL_PAD, y1 = n.row * CELL_H + port_y(n.op, i);
            const dx = Math.max(30, Math.abs(x1 - x0) * 0.5);
            x.strokeStyle = TYPE_COLORS[OPS[s.op].out];
            x.beginPath(); x.moveTo(x0, y0); x.bezierCurveTo(x0 + dx, y0, x1 - dx, y1, x1, y1); x.stroke();
        }
    // Nodes.
    for (const n of g.nodes) {
        const op = OPS[n.op], bx = n.col * CELL_W + CELL_PAD, by = n.row * CELL_H + CELL_PAD, bw = CELL_W - 2 * CELL_PAD, bh = CELL_H - 2 * CELL_PAD;
        x.fillStyle = '#2d3036'; x.fillRect(bx, by, bw, bh);
        x.fillStyle = TYPE_COLORS[op.out]; x.fillRect(bx, by, bw, 17);
        x.strokeStyle = n._err ? '#d0503c' : '#44474e'; x.lineWidth = 1; x.strokeRect(bx + 0.5, by + 0.5, bw - 1, bh - 1);
        x.font = 'bold 11px sans-serif'; x.fillStyle = '#f2f2f2'; x.textBaseline = 'middle';
        x.fillText(n.name, bx + 6, by + 9);
        if (A.preview_id === n.id) { x.fillStyle = '#ffd860'; x.fillText('◉', bx + bw - 14, by + 9); }
        x.font = '11px sans-serif'; x.fillStyle = '#b8bcc4';
        x.fillText(n.op + (op.chan ? '  · ' + op.chan : ''), bx + 10, by + 30);
        x.fillStyle = n._err ? '#e0705c' : n._cached ? '#7f8590' : '#9ad08a';
        const st = n._err ? (n._err === 'skipped' ? '' : 'error') : n._ms === undefined ? '' : n._cached ? 'cached' : n._ms.toFixed(1) + ' ms';
        x.fillText(st, bx + 10, by + bh - 10);
        for (let i = 0; i < op.ins.length; i++) {
            x.fillStyle = n.ins[i] ? TYPE_COLORS[op.ins[i]] : '#1c1d20';
            x.strokeStyle = TYPE_COLORS[op.ins[i]];
            x.beginPath(); x.arc(bx, n.row * CELL_H + port_y(n.op, i), 4, 0, Math.PI * 2); x.fill(); x.stroke();
        }
        if (op.out !== 'output') { x.fillStyle = TYPE_COLORS[op.out]; x.beginPath(); x.arc(bx + bw, by + bh / 2, 4, 0, Math.PI * 2); x.fill(); }
    }
    // Cursor.
    x.strokeStyle = E.mode === 'wire' ? '#5fd0ff' : E.mode === 'edit' ? '#ffd860' : '#e8e8e8';
    x.lineWidth = 2;
    x.strokeRect(E.cur[0] * CELL_W + 2, E.cur[1] * CELL_H + 2, CELL_W - 4, CELL_H - 4);
    if (E.mode === 'wire') {
        const s = node_find(g, E.wire_src);
        if (s) {
            x.setLineDash([5, 4]); x.strokeStyle = '#5fd0ff';
            x.beginPath(); x.moveTo(s.col * CELL_W + CELL_W - CELL_PAD, s.row * CELL_H + CELL_H / 2);
            x.lineTo(E.cur[0] * CELL_W + CELL_PAD, E.cur[1] * CELL_H + CELL_H / 2); x.stroke(); x.setLineDash([]);
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

function inspector_build(E)
{
    const A = E.A, el = E.insp, n = node_find(A.g, E.insp_node);
    el.innerHTML = '';
    if (!n) { el.innerHTML = '<div class="hint">Select a node (arrows / click).<br>Enter on an empty cell inserts a node.</div>'; return; }
    const op = OPS[n.op];
    const head = document.createElement('div');
    head.className = 'insp-head';
    head.innerHTML = '<input class="name" value="' + n.name + '" title="node name (part of the node seed)"> <span class="op">' + n.op + '</span>' +
        '<div class="desc">' + op.desc + '</div>' +
        '<div class="desc">seed channel: <b>' + (op.chan || '-') + '</b>' + (op.chan ? ' &nbsp; node seed ' + hex32(node_seed(A.seeds, op.chan, n.name)) : '') + '</div>' +
        (n._err && n._err !== 'skipped' ? '<div class="err">' + n._err + '</div>' : '');
    el.appendChild(head);
    head.querySelector('.name').addEventListener('change', ev => {
        const v = ev.target.value.trim().replace(/[^\w]/g, '_');
        if (!v || (node_by_name(A.g, v) && node_by_name(A.g, v) !== n)) { ev.target.value = n.name; return; }
        n.name = v; A.changed(true);
    });
    const tbl = document.createElement('table');
    tbl.className = 'params';
    tbl.innerHTML = '<tr><th>parameter</th><th>value (species)</th><th>variation</th><th>now</th></tr>';
    op.params.forEach(function (d, i) {
        const p = n.p[d.name], tr = document.createElement('tr');
        tr.dataset.row = i;
        if (E.mode === 'edit' && E.row === i) tr.className = 'sel';
        const td_name = document.createElement('td'); td_name.textContent = d.name; td_name.title = d.desc;
        const td_val = document.createElement('td'), td_var = document.createElement('td'), td_now = document.createElement('td');
        td_now.className = 'now';
        if (d.type === 'f' || d.type === 'i') {
            const rng = document.createElement('input');
            rng.type = 'range'; rng.min = d.min; rng.max = d.max; rng.step = d.step; rng.value = p.v;
            const num = document.createElement('input');
            num.type = 'number'; num.min = d.min; num.max = d.max; num.step = d.step; num.value = p.v; num.className = 'num';
            const set = function (v) { v = clamp(parseFloat(v), d.min, d.max); if (isNaN(v)) return; p.v = d.type === 'i' ? Math.round(v) : v; rng.value = p.v; num.value = p.v; A.changed(false); };
            rng.addEventListener('input', () => set(rng.value));
            num.addEventListener('change', () => set(num.value));
            td_val.appendChild(rng); td_val.appendChild(num);
            const vr = document.createElement('input');
            vr.type = 'number'; vr.min = 0; vr.step = d.step; vr.value = p.vari; vr.className = 'num';
            vr.addEventListener('change', () => { p.vari = Math.max(0, parseFloat(vr.value) || 0); A.changed(false); });
            td_var.appendChild(vr);
        } else if (d.type === 'e') {
            const s = document.createElement('select');
            d.labels.forEach((l, k) => { const o = document.createElement('option'); o.value = k; o.textContent = l; s.appendChild(o); });
            s.value = p.v;
            s.addEventListener('change', () => { p.v = parseInt(s.value, 10); A.changed(false); });
            td_val.appendChild(s);
        } else {
            const ci = document.createElement('input');
            ci.type = 'color'; ci.value = p.v;
            ci.addEventListener('input', () => { p.v = ci.value; A.changed(false); });
            td_val.appendChild(ci);
        }
        tr.appendChild(td_name); tr.appendChild(td_val); tr.appendChild(td_var); tr.appendChild(td_now);
        tr.addEventListener('mousedown', () => { E.row = i; });
        tbl.appendChild(tr);
    });
    el.appendChild(tbl);
    const ins = OPS[n.op].ins;
    if (ins.length) {
        const d = document.createElement('div');
        d.className = 'desc';
        d.innerHTML = 'inputs: ' + ins.map((t, i) => { const s = node_find(A.g, n.ins[i]); return t + ' ← ' + (s ? '<b>' + s.name + '</b>' : '<i>none</i>'); }).join(', ');
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
    const rows = E.insp.querySelectorAll('tr[data-row]');
    rows.forEach(tr => {
        const d = op.params[parseInt(tr.dataset.row, 10)], v = n._P[d.name];
        const td = tr.querySelector('.now');
        if (d.type === 'e') td.textContent = d.name === 'size' ? v : d.labels[v];
        else if (d.type === 'c') td.textContent = '';
        else td.textContent = d.type === 'i' ? String(v) : (Math.round(v * 1000) / 1000).toString();
    });
}

function inspector_key(E, e)
{
    const A = E.A, n = node_find(A.g, E.insp_node);
    if (!n) { editor_set_mode(E, 'nav'); return true; }
    const op = OPS[n.op], k = e.key;
    if (k === 'Escape') { editor_set_mode(E, 'nav'); return true; }
    if (!op.params.length) return k === 'Enter';
    const d = op.params[E.row], p = n.p[d.name];
    const sel = function () { E.insp.querySelectorAll('tr[data-row]').forEach(tr => tr.classList.toggle('sel', parseInt(tr.dataset.row, 10) === E.row)); };
    if (k === 'ArrowUp') { E.row = (E.row + op.params.length - 1) % op.params.length; sel(); return true; }
    if (k === 'ArrowDown') { E.row = (E.row + 1) % op.params.length; sel(); return true; }
    if (k === 'ArrowLeft' || k === 'ArrowRight') {
        const dir = k === 'ArrowLeft' ? -1 : 1, mul = e.shiftKey ? 10 : 1;
        if (d.type === 'e') p.v = (p.v + dir + d.labels.length) % d.labels.length;
        else if (d.type === 'f' || d.type === 'i') {
            if (e.altKey || e.ctrlKey || e.metaKey) p.vari = Math.max(0, Math.round((p.vari + dir * d.step * mul) * 1e5) / 1e5);
            else p.v = clamp(Math.round((p.v + dir * d.step * mul) * 1e5) / 1e5, d.min, d.max);
        } else return true;
        A.changed(false);
        inspector_build(E);
        return true;
    }
    if (k === 'Enter' && d.type === 'e') { p.v = (p.v + 1) % d.labels.length; A.changed(false); inspector_build(E); return true; }
    if (k === 'Enter' && d.type === 'c') { const ci = E.insp.querySelectorAll('tr[data-row]')[E.row].querySelector('input[type=color]'); if (ci) ci.click(); return true; }
    return false;
}
