// Deterministic random numbers and hashing. Everything is 32-bit integer arithmetic (Math.imul, >>> 0), so the same
// functions port 1:1 to C (u32) and give identical results on every platform.

function hash_u32(x)
{
    x = x >>> 0;
    x ^= x >>> 16; x = Math.imul(x, 0x7feb352d);
    x ^= x >>> 15; x = Math.imul(x, 0x846ca68b);
    x ^= x >>> 16;
    return x >>> 0;
}

function hash2(a, b) { return hash_u32((a >>> 0) ^ hash_u32((b + 0x9e3779b9) >>> 0)); }

// FNV-1a over the UTF-16 code units of a string.
function hash_str(s)
{
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 0x01000193);
    return h >>> 0;
}

// Hash of a byte view (typed array); used for the output hashes in the status bar.
function hash_bytes(u8, h)
{
    h = h === undefined ? 0x811c9dc5 : h;
    for (let i = 0; i < u8.length; i++) h = Math.imul(h ^ u8[i], 0x01000193);
    return h >>> 0;
}

// Counter-based stream: state advances by the golden-ratio constant, output = hash(state).
function rng_make(seed) { return { s: seed >>> 0 }; }
function rng_next(r) { r.s = (r.s + 0x9e3779b9) >>> 0; return hash_u32(r.s); }
function rng_f(r) { return rng_next(r) / 4294967296; }                 // [0, 1)
function rng_s(r) { return rng_f(r) * 2 - 1; }                         // [-1, 1)
function rng_range(r, a, b) { return a + (b - a) * rng_f(r); }
// Stream for element k of a node (branch k, leaf k, ...): independent of how many other elements exist.
function rng_sub(seed, k) { return rng_make(hash2(seed, k)); }

// Seed of a node: its channel's instance seed combined with the node's name.
function node_seed(seeds, channel, name)
{
    const s = channel ? seeds[channel] >>> 0 : 0;
    return hash2(s, hash_str(name));
}

function hex32(x) { return ('00000000' + (x >>> 0).toString(16)).slice(-8); }
