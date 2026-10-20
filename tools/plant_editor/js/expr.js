// Arithmetic expressions for the Expr node: numbers, variables x y z lod pi, + - * / ^ (power), parentheses and
// functions min max abs floor ceil round sqrt pow clamp lerp sin cos exp log. Compiled once into a closure tree;
// throws an Error with a readable message on syntax errors.

const EXPR_FUNCS = {
    min: [2, Math.min], max: [2, Math.max], abs: [1, Math.abs], floor: [1, Math.floor], ceil: [1, Math.ceil],
    round: [1, Math.round], sqrt: [1, Math.sqrt], pow: [2, Math.pow], sin: [1, Math.sin], cos: [1, Math.cos],
    exp: [1, Math.exp], log: [1, Math.log],
    clamp: [3, function (v, a, b) { return v < a ? a : v > b ? b : v; }],
    lerp: [3, function (a, b, t) { return a + (b - a) * t; }],
};
const EXPR_VARS = ['x', 'y', 'z', 'lod'];

function expr_compile(src)
{
    const toks = [], re = /\s*(?:(\d+\.?\d*(?:e[+-]?\d+)?|\.\d+)|([A-Za-z_]\w*)|(.))/gy;
    let m;
    while (re.lastIndex < src.length && (m = re.exec(src))) {
        if (m[1] !== undefined) toks.push({ t: 'n', v: parseFloat(m[1]) });
        else if (m[2] !== undefined) toks.push({ t: 'id', v: m[2] });
        else if (m[3] !== undefined && m[3].trim()) toks.push({ t: 'op', v: m[3] });
    }
    let i = 0;
    const peek = () => toks[i], eat = v => { if (toks[i] && toks[i].v === v) { i++; return true; } return false; };
    const fail = msg => { throw new Error('expression: ' + msg); };
    function primary() {
        const t = toks[i++];
        if (!t) fail('unexpected end');
        if (t.t === 'n') { const v = t.v; return () => v; }
        if (t.t === 'op' && t.v === '(') { const e = sum(); if (!eat(')')) fail('missing )'); return e; }
        if (t.t === 'op' && t.v === '-') { const e = power(); return s => -e(s); }
        if (t.t === 'id') {
            if (eat('(')) {
                const f = EXPR_FUNCS[t.v];
                if (!f) fail('unknown function ' + t.v);
                const args = [];
                if (!eat(')')) { do args.push(sum()); while (eat(',')); if (!eat(')')) fail('missing ) after arguments of ' + t.v); }
                if (args.length !== f[0]) fail(t.v + ' takes ' + f[0] + ' argument(s)');
                const fn = f[1];
                return s => fn.apply(null, args.map(a => a(s)));
            }
            if (t.v === 'pi') return () => Math.PI;
            const k = EXPR_VARS.indexOf(t.v);
            if (k < 0) fail('unknown variable ' + t.v + ' (use x, y, z, lod)');
            return s => s[k];
        }
        fail('unexpected ' + t.v);
    }
    function power() { const a = primary(); if (eat('^')) { const b = power(); return s => Math.pow(a(s), b(s)); } return a; }
    function product() {
        let a = power();
        for (;;) {
            if (eat('*')) { const l = a, r = power(); a = s => l(s) * r(s); }
            else if (eat('/')) { const l = a, r = power(); a = s => l(s) / r(s); }
            else return a;
        }
    }
    function sum() {
        let a = product();
        for (;;) {
            if (eat('+')) { const l = a, r = product(); a = s => l(s) + r(s); }
            else if (eat('-')) { const l = a, r = product(); a = s => l(s) - r(s); }
            else return a;
        }
    }
    const e = sum();
    if (i < toks.length) fail('unexpected ' + toks[i].v);
    return e;
}
