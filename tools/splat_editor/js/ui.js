// Panel widgets: slider bars (name and value in a framed bar, filled up to the value; drag inside, Ctrl = fine,
// double-click = type a value), colour rows, buttons. Same look as the plant editor.

function el(tag, cls, html) { const e = document.createElement(tag); if (cls) e.className = cls; if (html !== undefined) e.innerHTML = html; return e; }

// opts: { min, max, step, log (bool: logarithmic bar), get(), set(v, final) }
function ui_bar(label, opts)
{
    const bar = el('div', 'sbar'), fill = el('div', 'sfill'), txt = el('span', 'stext');
    bar.appendChild(fill); bar.appendChild(txt);
    const lo = opts.log ? Math.log(opts.min) : opts.min, hi = opts.log ? Math.log(opts.max) : opts.max;
    const to_u = v => (clamp(opts.log ? Math.log(Math.max(v, opts.min)) : v, lo, hi) - lo) / (hi - lo);
    const from_u = u => { const x = lo + clamp(u, 0, 1) * (hi - lo); return opts.log ? Math.exp(x) : x; };
    const snap = v => { v = clamp(v, opts.min, opts.max); return opts.step ? Math.round(v / opts.step) * opts.step : v; };
    const show = function () {
        const v = opts.get();
        fill.style.width = (to_u(v) * 100) + '%';
        const d = Math.abs(v) >= 100 ? 1 : Math.abs(v) >= 1 ? 3 : 5;
        txt.innerHTML = '<span>' + label + '</span><b>' + parseFloat(v.toFixed(d)) + '</b>';
    };
    bar.addEventListener('mousedown', ev => {
        if (ev.button !== 0) return;
        ev.preventDefault();
        if (ev.detail >= 2) return;
        const r = bar.getBoundingClientRect(), u0 = to_u(opts.get()), x0 = ev.clientX;
        bar._prev = opts.get();
        const at = e2 => from_u(e2.ctrlKey ? u0 + (e2.clientX - x0) / r.width * 0.05 : (e2.clientX - r.left) / r.width);
        if (!ev.ctrlKey) { opts.set(snap(at(ev)), false); show(); }
        bar.classList.add('drag');
        const move = e2 => { opts.set(snap(at(e2)), false); show(); };
        const up = () => { bar.classList.remove('drag'); window.removeEventListener('mousemove', move); window.removeEventListener('mouseup', up); opts.set(opts.get(), true); };
        window.addEventListener('mousemove', move); window.addEventListener('mouseup', up);
    });
    bar.addEventListener('dblclick', () => {
        if (bar._prev !== undefined) { opts.set(bar._prev, false); show(); }
        const ni = el('input', 'sedit');
        ni.type = 'number'; ni.step = opts.step || 'any'; ni.value = opts.get();
        bar.appendChild(ni); ni.focus(); ni.select();
        let closed = false;
        const done = ok => { if (closed) return; closed = true; if (ok && !isNaN(parseFloat(ni.value))) opts.set(snap(parseFloat(ni.value)), true); ni.remove(); show(); };
        ni.addEventListener('keydown', e2 => { if (e2.key === 'Enter') done(true); else if (e2.key === 'Escape') done(false); e2.stopPropagation(); });
        ni.addEventListener('blur', () => done(true));
    });
    show();
    bar._show = show;
    return bar;
}

function ui_color(label, get, set)
{
    const row = el('div', 'crow'), l = el('span', 'pname', label), ci = el('input');
    ci.type = 'color'; ci.value = get();
    ci.addEventListener('input', () => set(ci.value, false));
    ci.addEventListener('change', () => set(ci.value, true));
    row.appendChild(l); row.appendChild(ci);
    return row;
}

function ui_button(text, fn, title)
{
    const b = el('button', null, text);
    if (title) b.title = title;
    b.addEventListener('click', fn);
    return b;
}
