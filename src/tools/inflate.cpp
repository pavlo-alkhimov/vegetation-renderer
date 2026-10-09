// Minimal zlib/deflate decoder (RFC 1950/1951) for TIFF Deflate strips. Canonical-Huffman decoding in the style of
// Mark Adler's puff.c: simple and slow (~100 MB/s); fine for an offline cooker.

typedef struct {
    const u8* in; size_t in_size, in_pos;
    u8* out; size_t out_size, out_pos;
    u32 bitbuf, bitcnt;
    bool err;
} Inflate;

typedef struct { i16 count[16]; i16 symbol[288]; } Huffman;

static u32 inf_bits(Inflate* s, u32 need)
{
    u32 val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos == s->in_size) { s->err = true; return 0; }
        val |= (u32)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (u32)((u64)val >> need);
    s->bitcnt -= need;
    return val & ((1u << need) - 1);
}

static int inf_decode(Inflate* s, const Huffman* h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)inf_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = true;
    return -1;
}

static void inf_build(Huffman* h, const i16* lengths, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    i16 offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (i16)i;
    h->count[0] = 0;
}

static void inf_codes(Inflate* s, const Huffman* lencode, const Huffman* distcode)
{
    static const u16 lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const u16 lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const u16 dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                  4097,6145,8193,12289,16385,24577};
    static const u16 dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = inf_decode(s, lencode);
        if (s->err) return;
        if (sym < 256) {
            if (s->out_pos == s->out_size) { s->err = true; return; }
            s->out[s->out_pos++] = (u8)sym;
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = true; return; }
            size_t len = lbase[sym] + inf_bits(s, lext[sym]);
            int dsym = inf_decode(s, distcode);
            if (s->err || dsym >= 30) { s->err = true; return; }
            size_t dist = dbase[dsym] + inf_bits(s, dext[dsym]);
            if (dist > s->out_pos || s->out_pos + len > s->out_size) { s->err = true; return; }
            for (size_t i = 0; i < len; i++, s->out_pos++) s->out[s->out_pos] = s->out[s->out_pos - dist];
        }
    }
}

static void inf_fixed(Inflate* s)
{
    static Huffman lencode, distcode;
    static bool built;
    if (!built) {
        i16 lengths[288];
        int i = 0;
        for (; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        inf_build(&lencode, lengths, 288);
        for (i = 0; i < 30; i++) lengths[i] = 5;
        inf_build(&distcode, lengths, 30);
        built = true;   // racy but idempotent: every thread writes identical tables
    }
    inf_codes(s, &lencode, &distcode);
}

static void inf_dynamic(Inflate* s)
{
    static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = (int)inf_bits(s, 5) + 257, ndist = (int)inf_bits(s, 5) + 1, ncode = (int)inf_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) { s->err = true; return; }
    i16 lengths[320] = {};
    for (int i = 0; i < ncode; i++) lengths[order[i]] = (i16)inf_bits(s, 3);
    Huffman lencode, distcode;
    inf_build(&lencode, lengths, 19);
    for (int i = 0; i < nlen + ndist;) {
        int sym = inf_decode(s, &lencode);
        if (s->err) return;
        if (sym < 16) { lengths[i++] = (i16)sym; continue; }
        i16 len = 0;
        int rep;
        if (sym == 16) { if (i == 0) { s->err = true; return; } len = lengths[i - 1]; rep = 3 + (int)inf_bits(s, 2); }
        else if (sym == 17) rep = 3 + (int)inf_bits(s, 3);
        else rep = 11 + (int)inf_bits(s, 7);
        if (i + rep > nlen + ndist) { s->err = true; return; }
        while (rep--) lengths[i++] = len;
    }
    inf_build(&lencode, lengths, nlen);
    inf_build(&distcode, lengths + nlen, ndist);
    inf_codes(s, &lencode, &distcode);
}

// zlib stream (or raw deflate if the header does not parse) -> out. Returns bytes written, or -1 on error.
static i64 inflate_zlib(const u8* in, size_t in_size, u8* out, size_t out_size)
{
    Inflate s = {};
    s.in = in; s.in_size = in_size; s.out = out; s.out_size = out_size;
    if (in_size >= 2 && (in[0] & 0x0f) == 8 && ((in[0] << 8) | in[1]) % 31 == 0) s.in_pos = 2;
    for (bool last = false; !last && !s.err;) {
        last = inf_bits(&s, 1) != 0;
        u32 type = inf_bits(&s, 2);
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.in_pos + 4 > s.in_size) return -1;
            u32 len = s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8);
            s.in_pos += 4;
            if (s.in_pos + len > s.in_size || s.out_pos + len > s.out_size) return -1;
            memcpy(s.out + s.out_pos, s.in + s.in_pos, len);
            s.in_pos += len; s.out_pos += len;
        } else if (type == 1) inf_fixed(&s);
        else if (type == 2) inf_dynamic(&s);
        else return -1;
    }
    return s.err ? -1 : (i64)s.out_pos;
}
