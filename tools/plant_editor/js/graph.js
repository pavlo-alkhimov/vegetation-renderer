// Node graph: nodes pinned to table cells, typed single-output nodes, wires as input references.
// Evaluation is pull-based from a target node with a per-evaluation memo and a cross-evaluation content cache.
//
// Graph = { nodes: [Node] }; Node = { id, name, op, col, row, p: { param: { v, vari } }, ins: [node id or 0],
//   binds: { param or 'seed': value ref } }.
// A reference to a node output is ref = id + port * 65536 (port 0 for single-output nodes; multi-output nodes declare
// op.outs, e.g. the Inputs node). node_find accepts refs.

function REF(id, port) { return id + (port || 0) * 65536; }
function ref_id(r) { return r & 0xffff; }
function ref_port(r) { return r >>> 16; }
function ref_name(g, r) { const n = node_find(g, r); if (!n) return '-'; const o = OPS[n.op].outs; return o ? n.name + '.' + o[ref_port(r)] : n.name; }
// The value a ref delivers from its node's result.
function ref_value(g, r, val) { const n = node_find(g, r); return n && OPS[n.op].outs ? val[ref_port(r)] : val; }
function node_span(n) { return OPS[n.op].span || 1; }

function graph_new() { return { nodes: [], next_id: 1 }; }

function node_find(g, id) { id = id & 0xffff; for (const n of g.nodes) if (n.id === id) return n; return null; }
function node_by_name(g, name) { for (const n of g.nodes) if (n.name === name) return n; return null; }
function node_at(g, col, row) { for (const n of g.nodes) if (n.col === col && row >= n.row && row < n.row + node_span(n)) return n; return null; }

function unique_name(g, base)
{
    base = base.toLowerCase();
    for (let i = 1; ; i++) { const nm = i === 1 && !node_by_name(g, base) ? base : base + i; if (!node_by_name(g, nm)) return nm; }
}

function node_add(g, op, col, row, name)
{
    const n = { id: g.next_id++, name: name || unique_name(g, op), op: op, col: col, row: row,
                p: node_defaults(op), ins: OPS[op].ins.map(() => 0), binds: {} };
    g.nodes.push(n);
    return n;
}

function node_remove(g, id)
{
    g.nodes = g.nodes.filter(n => n.id !== id);
    for (const n of g.nodes) {
        n.ins = n.ins.map(x => ref_id(x) === id ? 0 : x);
        for (const k of Object.keys(n.binds)) if (ref_id(n.binds[k]) === id) delete n.binds[k];
    }
}

// True if 'b' is upstream of (or equal to) 'a'.
function depends_on(g, a, b)
{
    if (ref_id(a) === ref_id(b)) return true;
    const n = node_find(g, a);
    if (!n) return false;
    for (const i of n.ins) if (i && depends_on(g, i, b)) return true;
    for (const k of Object.keys(n.binds)) if (depends_on(g, n.binds[k], b)) return true;
    return false;
}

// Connects src's output to dst's input slot; returns an error string or null.
function connect(g, src_id, dst_id, slot)
{
    const s = node_find(g, src_id), d = node_find(g, dst_id);
    if (!s || !d) return 'no node';
    const want = OPS[d.op].ins[slot];
    if (want === undefined) return 'no such input';
    if (OPS[s.op].out !== want) return d.name + ' input ' + slot + ' needs ' + want + ', ' + s.name + ' gives ' + OPS[s.op].out;
    if (depends_on(g, src_id, dst_id)) return 'connection would create a cycle';
    d.ins[slot] = src_id;
    return null;
}

// Parameters of dst that a value can drive: numeric parameters, plus 'seed' for nodes with a seed channel.
function bindable_params(op)
{
    const out = OPS[op].params.filter(d => d.type === 'f' || d.type === 'i').map(d => d.name);
    if (OPS[op].chan) out.push('seed');
    return out;
}

// Binds a value node to a parameter (or the seed) of dst; returns an error string or null.
function bind(g, src_id, dst_id, param)
{
    const s = node_find(g, src_id), d = node_find(g, dst_id);
    if (!s || !d) return 'no node';
    if (OPS[s.op].out !== 'value') return s.name + ' is not a value';
    if (bindable_params(d.op).indexOf(param) < 0) return d.name + ' has no numeric parameter ' + param;
    if (depends_on(g, src_id, dst_id)) return 'binding would create a cycle';
    d.binds[param] = src_id;
    return null;
}

// Picks the input slot for a connection src -> dst: the first free compatible one, else the first compatible.
function pick_slot(g, src_id, dst_id)
{
    const s = node_find(g, src_id), d = node_find(g, dst_id), ins = OPS[d.op].ins;
    let first = -1;
    for (let i = 0; i < ins.length; i++) {
        if (ins[i] !== OPS[s.op].out) continue;
        if (first < 0) first = i;
        if (!d.ins[i]) return i;
    }
    return first;
}

// Context: { seeds, cache: Map, used: Set, memo: Map, no_textures }.
function eval_ctx(seeds, cache, lod) { return { seeds: seeds, cache: cache, lod: lod || 0, used: new Set(), memo: new Map(), no_textures: false }; }

function eval_node(g, id, ctx)
{
    if (ctx.memo.has(id)) return ctx.memo.get(id);
    const n = node_find(g, id), op = OPS[n.op];
    const res = { val: null, key: 0, err: null, ms: 0, cached: false };
    ctx.memo.set(id, res);
    const ins = [], keys = [];
    for (let i = 0; i < op.ins.length; i++) {
        if (!n.ins[i]) {
            if (!(op.optional && op.optional[i])) res.err = 'input ' + (i + 1) + ' (' + op.ins[i] + ') not connected';
            ins.push(null); keys.push(0);
            continue;
        }
        const r = eval_node(g, ref_id(n.ins[i]), ctx);
        if (r.err && !(op.optional && op.optional[i])) res.err = 'input ' + (i + 1) + ' (' + node_find(g, n.ins[i]).name + ') has no result';
        ins.push(r.err ? null : ref_value(g, n.ins[i], r.val)); keys.push(r.err ? 0 : r.key + '.' + ref_port(n.ins[i]));
    }
    const bvals = {};
    for (const k of Object.keys(n.binds)) {
        const r = eval_node(g, ref_id(n.binds[k]), ctx);
        if (r.err) { res.err = k + ' <- ' + node_find(g, n.binds[k]).name + ' has no result'; break; }
        bvals[k] = ref_value(g, n.binds[k], r.val); keys.push(k + ':' + r.key + '.' + ref_port(n.binds[k]));
    }
    if (res.err) { n._err = res.err; n._ms = 0; n._cached = false; return res; }
    if (op.texture && ctx.no_textures) { res.val = null; res.err = 'skipped'; return res; }
    // Nodes that use random numbers (seed channel set) take their seed only from an explicit value input.
    if (op.chan && bvals.seed === undefined) { res.err = 'seed not connected (bind an Inputs seed or other value to seed)'; n._err = res.err; n._ms = 0; n._cached = false; return res; }
    const seed = op.chan ? hash2(bvals.seed >>> 0, hash_str(n.name)) : 0;
    const P = resolve_params(n, seed, ctx.lod, bvals);
    res.key = hash2(hash_str(n.op + '/' + op.ver + '/' + JSON.stringify(P) + '/' + keys.join(',') + (op.lod ? '/lod' + ctx.lod : '') + (op.seeds ? '/' + JSON.stringify(ctx.seeds) : '')), seed);
    if (ctx.cache.has(res.key)) {
        res.val = ctx.cache.get(res.key); res.cached = true;
    } else {
        const t0 = performance.now();
        try { res.val = op.fn(P, ins, seed, ctx); }
        catch (e) { res.err = String(e && e.message || e); console.error(e); }
        res.ms = performance.now() - t0;
        if (!res.err) ctx.cache.set(res.key, res.val);
    }
    ctx.used.add(res.key);
    n._err = res.err; n._ms = res.ms; n._cached = res.cached; n._P = P; n._seed = op.chan ? seed : undefined; n._val = op.out === 'value' ? res.val : undefined;
    n._tex = op.out === 'texture' && !res.err ? res.val : n._tex;
    return res;
}

// Drops cache entries not used by the last evaluation (keeps memory bounded).
function cache_trim(cache, used)
{
    for (const k of Array.from(cache.keys())) if (!used.has(k)) cache.delete(k);
}

function graph_output(g)
{
    for (const n of g.nodes) if (n.op === 'Output') return n;
    return null;
}

// Free cell nearest to (col, row), searching rings outward.
function nearest_free_cell(g, col, row)
{
    for (let r = 0; r < 64; r++)
        for (let dy = -r; dy <= r; dy++)
            for (let dx = -r; dx <= r; dx++) {
                if (Math.max(Math.abs(dx), Math.abs(dy)) !== r) continue;
                const c = col + dx, w = row + dy;
                if (c < 0 || w < 0) continue;
                if (!node_at(g, c, w)) return [c, w];
            }
    return [col, row];
}
