#include "image.h"
#include "ramfs.h"
#include "serial.h"

/* ====== STATIC BUFFERS ====== */
/* Pool of pixel buffers so multiple images can be open simultaneously */
#define IMG_SLOT_W 512
#define IMG_SLOT_H 512
#define IMG_SLOT_SIZE (IMG_SLOT_W * IMG_SLOT_H)
#define IMG_SLOTS 4
static uint32_t img_pool[IMG_SLOTS][IMG_SLOT_SIZE]; /* 4 * 1MB = 4MB */
static int img_next_slot = 0;

/* Get next available pixel buffer (round-robin) */
static uint32_t* img_alloc_pixels(void) {
    uint32_t* p = img_pool[img_next_slot];
    img_next_slot = (img_next_slot + 1) % IMG_SLOTS;
    return p;
}

/* Also keep one large buffer for bigger images */
static uint32_t img_pixels[IMG_MAX_W * IMG_MAX_H];   /* 16MB decoded pixels */
static uint8_t inflate_out[20 * 1024 * 1024];         /* 20MB decompressed data */
static uint8_t idat_buf[8 * 1024 * 1024];             /* 8MB collected IDAT */

/* ====== HELPERS ====== */
static uint16_t rd16le(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t rd32le(const uint8_t* p) { return p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24); }
static uint32_t rd32be(const uint8_t* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

static bool ends_with(const char* s, const char* suf) {
    int sl = strlen(s), ul = strlen(suf);
    if (sl < ul) return false;
    for (int i = 0; i < ul; i++)
        if (tolower(s[sl - ul + i]) != tolower(suf[i])) return false;
    return true;
}

bool image_is_image(const char* name) {
    return ends_with(name, ".bmp") || ends_with(name, ".tga") ||
           ends_with(name, ".png") || ends_with(name, ".ppm");
}

/* ====== BMP DECODER ====== */
bool image_load_bmp(image_t* img, const uint8_t* data, uint32_t size) {
    if (size < 54 || data[0] != 'B' || data[1] != 'M') return false;

    uint32_t offset = rd32le(data + 10);
    int32_t w = (int32_t)rd32le(data + 18);
    int32_t h = (int32_t)rd32le(data + 22);
    uint16_t bpp = rd16le(data + 28);
    uint32_t comp = rd32le(data + 30);

    if (comp != 0 && comp != 3) return false;  /* Only uncompressed + bitfields */
    if (bpp != 24 && bpp != 32) return false;
    bool flip = (h > 0);
    if (h < 0) h = -h;
    if (w <= 0 || w > IMG_MAX_W || h > IMG_MAX_H) return false;

    int row_bytes = ((w * bpp / 8) + 3) & ~3;  /* Rows padded to 4 bytes */

    img->width = w;
    img->height = h;
    img->pixels = img_pixels;
    img->valid = true;

    for (int y = 0; y < h; y++) {
        int src_y = flip ? (h - 1 - y) : y;
        const uint8_t* row = data + offset + src_y * row_bytes;
        if (offset + src_y * row_bytes + w * (bpp / 8) > size) break;
        for (int x = 0; x < w; x++) {
            const uint8_t* p = row + x * (bpp / 8);
            img->pixels[y * w + x] = (p[2] << 16) | (p[1] << 8) | p[0];
        }
    }
    return true;
}

/* ====== TGA DECODER ====== */
bool image_load_tga(image_t* img, const uint8_t* data, uint32_t size) {
    if (size < 18) return false;
    uint8_t id_len = data[0];
    uint8_t img_type = data[2];
    uint16_t w = rd16le(data + 12);
    uint16_t h = rd16le(data + 14);
    uint8_t bpp = data[16];
    uint8_t desc = data[17];

    if (w == 0 || h == 0 || w > IMG_MAX_W || h > IMG_MAX_H) return false;
    if (img_type != 2 && img_type != 10) return false;  /* Uncompressed or RLE RGB */
    if (bpp != 24 && bpp != 32) return false;

    int bytes_pp = bpp / 8;
    bool top_origin = (desc & 0x20) != 0;
    const uint8_t* src = data + 18 + id_len;
    uint32_t src_left = size - 18 - id_len;

    img->width = w; img->height = h;
    img->pixels = img_pixels; img->valid = true;

    if (img_type == 2) {
        /* Uncompressed */
        for (int y = 0; y < h; y++) {
            int dy = top_origin ? y : (h - 1 - y);
            for (int x = 0; x < w; x++) {
                uint32_t off = (dy * w + x) * bytes_pp;
                if (off + bytes_pp > src_left) return true;
                const uint8_t* p = src + off;
                img->pixels[y * w + x] = (p[2] << 16) | (p[1] << 8) | p[0];
            }
        }
    } else {
        /* RLE compressed */
        int px = 0, total = w * h;
        uint32_t si = 0;
        while (px < total && si < src_left) {
            uint8_t hdr = src[si++];
            int count = (hdr & 0x7F) + 1;
            if (hdr & 0x80) {
                /* RLE packet */
                if (si + bytes_pp > src_left) break;
                uint32_t col = (src[si+2]<<16)|(src[si+1]<<8)|src[si];
                si += bytes_pp;
                for (int i = 0; i < count && px < total; i++) {
                    int y = top_origin ? (px / w) : (h - 1 - px / w);
                    img->pixels[y * w + (px % w)] = col;
                    px++;
                }
            } else {
                /* Raw packet */
                for (int i = 0; i < count && px < total; i++) {
                    if (si + bytes_pp > src_left) break;
                    uint32_t col = (src[si+2]<<16)|(src[si+1]<<8)|src[si];
                    si += bytes_pp;
                    int y = top_origin ? (px / w) : (h - 1 - px / w);
                    img->pixels[y * w + (px % w)] = col;
                    px++;
                }
            }
        }
    }
    return true;
}

/* ====== DEFLATE DECOMPRESSOR ====== */

/* Bit reader - LSB first (DEFLATE convention) */
typedef struct {
    const uint8_t* src;
    uint32_t len, pos;
    uint32_t buf;
    int cnt;
} bits_t;

static void bits_init(bits_t* b, const uint8_t* src, uint32_t len) {
    b->src = src; b->len = len; b->pos = 0; b->buf = 0; b->cnt = 0;
}

static uint32_t bits_read(bits_t* b, int n) {
    while (b->cnt < n) {
        uint8_t byte = (b->pos < b->len) ? b->src[b->pos++] : 0;
        b->buf |= (uint32_t)byte << b->cnt;
        b->cnt += 8;
    }
    uint32_t val = b->buf & ((1u << n) - 1);
    b->buf >>= n;
    b->cnt -= n;
    return val;
}

/* Huffman tree: array-based binary tree */
#define HT_MAX 8192
typedef struct {
    int16_t ch[HT_MAX][2]; /* children: >=0 internal node, -1 empty, <=-2 leaf */
    int n;
} htree_t;

/* Encoding: child >= 0 means internal node index.
 * child == -1 means empty (no code assigned).
 * child <= -2 means leaf: symbol = -(child + 2).
 * So symbol 0 = -2, symbol 1 = -3, etc. */
#define HT_EMPTY (-1)
#define HT_LEAF(sym) (-(sym) - 2)
#define HT_SYM(child) (-(child) - 2)

static void ht_init(htree_t* t) { t->n = 1; t->ch[0][0] = t->ch[0][1] = HT_EMPTY; }

static void ht_insert(htree_t* t, uint32_t code, int len, int sym) {
    int node = 0;
    /* Traverse all bits except the last one, creating internal nodes */
    for (int i = len - 1; i > 0; i--) {
        int bit = (code >> i) & 1;
        if (t->ch[node][bit] < 0) {
            int nn = t->n++;
            if (nn >= HT_MAX) {
                serial_printf("PNG: Huffman tree overflow at %d nodes (sym=%d len=%d)\n",
                              nn, sym, len);
                return;
            }
            t->ch[nn][0] = t->ch[nn][1] = HT_EMPTY;
            t->ch[node][bit] = nn;
        }
        node = t->ch[node][bit];
    }
    /* Last bit: store leaf directly in parent's child slot */
    int bit = (code >> 0) & 1;
    t->ch[node][bit] = HT_LEAF(sym);
}

static int ht_decode(htree_t* t, bits_t* b) {
    int node = 0;
    for (int depth = 0; depth < 16; depth++) {
        int bit = bits_read(b, 1);
        int child = t->ch[node][bit];
        if (child == HT_EMPTY) return -1;          /* Invalid path */
        if (child < HT_EMPTY) return HT_SYM(child); /* Leaf: return symbol */
        node = child;                               /* Internal node: continue */
    }
    return -1;  /* Code too long */
}

/* Build Huffman tree from array of code lengths (canonical codes) */
static void ht_build(htree_t* t, const uint8_t* lens, int count) {
    ht_init(t);
    int bl_count[16] = {0};
    for (int i = 0; i < count; i++)
        if (lens[i] > 0 && lens[i] <= 15) bl_count[lens[i]]++;

    uint32_t next_code[16];
    next_code[0] = 0;
    for (int bits = 1; bits <= 15; bits++)
        next_code[bits] = (next_code[bits-1] + bl_count[bits-1]) << 1;

    for (int i = 0; i < count; i++) {
        if (lens[i] > 0) {
            ht_insert(t, next_code[lens[i]], lens[i], i);
            next_code[lens[i]]++;
        }
    }
}

/* Fixed Huffman trees for DEFLATE */
static htree_t fixed_lit, fixed_dist;
static bool fixed_built = false;

static void build_fixed_trees(void) {
    if (fixed_built) return;
    uint8_t lit_lens[288], dist_lens[32];
    int i;
    for (i = 0; i <= 143; i++) lit_lens[i] = 8;
    for (; i <= 255; i++)      lit_lens[i] = 9;
    for (; i <= 279; i++)      lit_lens[i] = 7;
    for (; i <= 287; i++)      lit_lens[i] = 8;
    ht_build(&fixed_lit, lit_lens, 288);

    for (i = 0; i < 32; i++) dist_lens[i] = 5;
    ht_build(&fixed_dist, dist_lens, 32);
    fixed_built = true;
}

/* Length and distance base values + extra bits tables */
static const uint16_t len_base[] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
    35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};
static const uint8_t len_extra[] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
    3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};
static const uint16_t dist_base[] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073,
    4097,6145,8193,12289, 16385,24577
};
static const uint8_t dist_extra[] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6,
    7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};

/* Decode a Huffman-compressed block */
static int decode_block(bits_t* b, htree_t* lt, htree_t* dt,
                        uint8_t* out, int opos, int omax) {
    for (;;) {
        int sym = ht_decode(lt, b);
        if (sym < 0 || sym > 285) return -1;
        if (sym < 256) {
            if (opos < omax) out[opos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return opos;
        } else {
            int li = sym - 257;
            int length = len_base[li] + bits_read(b, len_extra[li]);
            int di = ht_decode(dt, b);
            if (di < 0 || di > 29) return -1;
            int distance = dist_base[di] + bits_read(b, dist_extra[di]);
            for (int i = 0; i < length && opos < omax; i++) {
                out[opos] = out[opos - distance];
                opos++;
            }
        }
    }
}

/* Code length alphabet order for dynamic Huffman */
static const uint8_t cl_order[] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* Main DEFLATE decompressor.
   Input: raw deflate stream (no zlib/gzip header).
   Returns decompressed size, or -1 on error. */
static int deflate_decompress(const uint8_t* src, uint32_t slen,
                              uint8_t* out, int omax) {
    bits_t b;
    bits_init(&b, src, slen);
    build_fixed_trees();

    int opos = 0;
    int bfinal;
    int block_num = 0;
    do {
        bfinal = bits_read(&b, 1);
        int btype = bits_read(&b, 2);

        if (btype == 0) {
            /* Stored block */
            b.buf = 0; b.cnt = 0;  /* Align to byte */
            if (b.pos + 4 > b.len) { serial_printf("DEFLATE: stored block truncated at block %d\n", block_num); return -1; }
            uint16_t len = b.src[b.pos] | (b.src[b.pos+1] << 8);
            b.pos += 4;  /* skip len + nlen */
            for (int i = 0; i < len && opos < omax; i++) {
                out[opos++] = (b.pos < b.len) ? b.src[b.pos++] : 0;
            }
        } else if (btype == 1) {
            /* Fixed Huffman */
            opos = decode_block(&b, &fixed_lit, &fixed_dist, out, opos, omax);
            if (opos < 0) { serial_printf("DEFLATE: fixed block decode failed at block %d\n", block_num); return -1; }
        } else if (btype == 2) {
            /* Dynamic Huffman */
            int hlit = bits_read(&b, 5) + 257;
            int hdist = bits_read(&b, 5) + 1;
            int hclen = bits_read(&b, 4) + 4;

            uint8_t cl_lens[19] = {0};
            for (int i = 0; i < hclen; i++)
                cl_lens[cl_order[i]] = bits_read(&b, 3);

            static htree_t cl_tree;
            ht_build(&cl_tree, cl_lens, 19);

            uint8_t all_lens[320] = {0};
            int total = hlit + hdist;
            int ai = 0;
            while (ai < total) {
                int sym = ht_decode(&cl_tree, &b);
                if (sym < 0) { serial_printf("DEFLATE: code length decode failed at block %d, ai=%d/%d\n", block_num, ai, total); return -1; }
                if (sym < 16) {
                    all_lens[ai++] = sym;
                } else if (sym == 16) {
                    int rep = bits_read(&b, 2) + 3;
                    uint8_t prev = (ai > 0) ? all_lens[ai-1] : 0;
                    for (int i = 0; i < rep && ai < total; i++) all_lens[ai++] = prev;
                } else if (sym == 17) {
                    int rep = bits_read(&b, 3) + 3;
                    for (int i = 0; i < rep && ai < total; i++) all_lens[ai++] = 0;
                } else if (sym == 18) {
                    int rep = bits_read(&b, 7) + 11;
                    for (int i = 0; i < rep && ai < total; i++) all_lens[ai++] = 0;
                } else {
                    serial_printf("DEFLATE: invalid code length sym %d at block %d\n", sym, block_num);
                    return -1;
                }
            }

            static htree_t dyn_lit, dyn_dist;
            ht_build(&dyn_lit, all_lens, hlit);
            ht_build(&dyn_dist, all_lens + hlit, hdist);
            serial_printf("DEFLATE: block %d dynamic hlit=%d hdist=%d lit_nodes=%d dist_nodes=%d\n",
                          block_num, hlit, hdist, dyn_lit.n, dyn_dist.n);
            opos = decode_block(&b, &dyn_lit, &dyn_dist, out, opos, omax);
            if (opos < 0) { serial_printf("DEFLATE: dynamic block decode failed at block %d\n", block_num); return -1; }
        } else {
            serial_printf("DEFLATE: invalid block type %d at block %d\n", btype, block_num);
            return -1;
        }
        block_num++;
    } while (!bfinal);

    serial_printf("DEFLATE: done, %d blocks, %d bytes output\n", block_num, opos);
    return opos;
}

/* Zlib wrapper: strip 2-byte header, pass to deflate */
static int zlib_decompress(const uint8_t* src, uint32_t slen,
                           uint8_t* out, int omax) {
    if (slen < 2) return -1;
    /* Check CMF: CM should be 8 (deflate), CINFO <= 7 */
    if ((src[0] & 0x0F) != 8) return -1;
    /* Skip 2-byte zlib header; pass ALL remaining bytes to deflate.
     * Deflate stops at BFINAL block; trailing Adler32 checksum is ignored. */
    return deflate_decompress(src + 2, slen - 2, out, omax);
}

/* ====== PNG DECODER ====== */

static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};

/* PNG filter reconstruction */
static uint8_t paeth_predict(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p - a; if (pa < 0) pa = -pa;
    int pb = p - b; if (pb < 0) pb = -pb;
    int pc = p - c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

bool image_load_png(image_t* img, const uint8_t* data, uint32_t size) {
    if (size < 8 || memcmp(data, png_sig, 8) != 0) return false;

    uint32_t pos = 8;
    int w = 0, h = 0, depth = 0, ctype = 0;
    uint8_t palette[256][3];
    int pal_count = 0;
    uint32_t idat_len = 0;
    bool got_ihdr = false;

    serial_printf("PNG: loading %u bytes\n", size);

    /* Parse chunks */
    while (pos + 12 <= size) {
        uint32_t clen = rd32be(data + pos);
        const uint8_t* ctype_tag = data + pos + 4;
        const uint8_t* cdata = data + pos + 8;

        if (pos + 12 + clen > size) break;

        if (memcmp(ctype_tag, "IHDR", 4) == 0 && clen >= 13) {
            w = rd32be(cdata);
            h = rd32be(cdata + 4);
            depth = cdata[8];
            ctype = cdata[9];
            serial_printf("PNG: IHDR %dx%d depth=%d ctype=%d interlace=%d\n",
                          w, h, depth, ctype, cdata[12]);
            if (cdata[12] != 0) { serial_printf("PNG: interlaced - unsupported\n"); return false; }
            if (depth != 8) { serial_printf("PNG: depth %d - unsupported\n", depth); return false; }
            if (w <= 0 || h <= 0 || w > IMG_MAX_W || h > IMG_MAX_H) {
                serial_printf("PNG: dimensions %dx%d exceed max %dx%d\n", w, h, IMG_MAX_W, IMG_MAX_H);
                return false;
            }
            got_ihdr = true;
        } else if (memcmp(ctype_tag, "PLTE", 4) == 0) {
            pal_count = clen / 3;
            if (pal_count > 256) pal_count = 256;
            for (int i = 0; i < pal_count; i++) {
                palette[i][0] = cdata[i*3];
                palette[i][1] = cdata[i*3+1];
                palette[i][2] = cdata[i*3+2];
            }
        } else if (memcmp(ctype_tag, "IDAT", 4) == 0) {
            if (idat_len + clen <= sizeof(idat_buf)) {
                memcpy(idat_buf + idat_len, cdata, clen);
                idat_len += clen;
            } else {
                serial_printf("PNG: IDAT overflow %u + %u > %u\n",
                              idat_len, clen, (uint32_t)sizeof(idat_buf));
            }
        } else if (memcmp(ctype_tag, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }

    if (!got_ihdr || idat_len == 0) {
        serial_printf("PNG: missing IHDR or IDAT (ihdr=%d idat_len=%u)\n", got_ihdr, idat_len);
        return false;
    }

    serial_printf("PNG: collected %u bytes of IDAT data\n", idat_len);

    /* Determine bytes per pixel */
    int bpp;
    switch (ctype) {
        case 0: bpp = 1; break;        /* Grayscale */
        case 2: bpp = 3; break;        /* RGB */
        case 3: bpp = 1; break;        /* Palette */
        case 4: bpp = 2; break;        /* Gray+Alpha */
        case 6: bpp = 4; break;        /* RGBA */
        default: return false;
    }

    /* Decompress */
    int raw_size = h * (1 + w * bpp);
    if (raw_size > (int)sizeof(inflate_out)) {
        serial_printf("PNG: raw_size %d exceeds inflate_out buffer %u\n",
                      raw_size, (uint32_t)sizeof(inflate_out));
        return false;
    }

    serial_printf("PNG: decompressing %u bytes, expecting %d raw bytes (bpp=%d)\n",
                  idat_len, raw_size, bpp);
    int dlen = zlib_decompress(idat_buf, idat_len, inflate_out, sizeof(inflate_out));
    serial_printf("PNG: zlib_decompress returned %d (need %d)\n", dlen, raw_size);
    if (dlen < 0) { serial_printf("PNG: decompression FAILED\n"); return false; }
    if (dlen < raw_size) { serial_printf("PNG: decompressed too small %d < %d\n", dlen, raw_size); return false; }

    /* Reconstruct filters and convert to pixels */
    img->width = w;
    img->height = h;
    img->pixels = img_pixels;
    img->valid = true;

    int stride = w * bpp;
    static uint8_t row_buf[IMG_MAX_W * 4 + 1];
    static uint8_t prev_buf[IMG_MAX_W * 4 + 1];
    memset(prev_buf, 0, stride);

    int rp = 0;  /* Read position in inflate_out */
    for (int y = 0; y < h; y++) {
        uint8_t filter = inflate_out[rp++];
        uint8_t* cur = row_buf;
        const uint8_t* raw = inflate_out + rp;
        rp += stride;

        /* Apply filter */
        for (int x = 0; x < stride; x++) {
            uint8_t a = (x >= bpp) ? cur[x - bpp] : 0;
            uint8_t b = prev_buf[x];
            uint8_t c = (x >= bpp) ? prev_buf[x - bpp] : 0;
            uint8_t r = raw[x];

            switch (filter) {
                case 0: cur[x] = r; break;
                case 1: cur[x] = r + a; break;
                case 2: cur[x] = r + b; break;
                case 3: cur[x] = r + ((a + b) >> 1); break;
                case 4: cur[x] = r + paeth_predict(a, b, c); break;
                default: cur[x] = r; break;
            }
        }

        /* Convert to RGB pixels */
        for (int x = 0; x < w; x++) {
            uint32_t pixel;
            switch (ctype) {
                case 0: { /* Grayscale */
                    uint8_t v = cur[x];
                    pixel = (v << 16) | (v << 8) | v;
                    break;
                }
                case 2: /* RGB */
                    pixel = (cur[x*3] << 16) | (cur[x*3+1] << 8) | cur[x*3+2];
                    break;
                case 3: { /* Palette */
                    uint8_t idx = cur[x];
                    if (idx < pal_count)
                        pixel = (palette[idx][0]<<16)|(palette[idx][1]<<8)|palette[idx][2];
                    else
                        pixel = 0;
                    break;
                }
                case 4: { /* Gray+Alpha */
                    uint8_t v = cur[x*2];
                    pixel = (v << 16) | (v << 8) | v;
                    break;
                }
                case 6: /* RGBA */
                    pixel = (cur[x*4] << 16) | (cur[x*4+1] << 8) | cur[x*4+2];
                    break;
                default:
                    pixel = 0;
            }
            img->pixels[y * w + x] = pixel;
        }
        memcpy(prev_buf, cur, stride);
    }
    serial_printf("PNG: successfully decoded %dx%d image\n", w, h);
    return true;
}

/* ====== AUTO-DETECT LOADER ====== */
bool image_load(image_t* img, const uint8_t* data, uint32_t size, const char* name) {
    img->valid = false;

    serial_printf("image_load: '%s' size=%u bytes\n", name ? name : "(null)", size);

    /* Try by magic bytes first */
    bool ok = false;
    if (size >= 8 && memcmp(data, png_sig, 8) == 0) {
        serial_printf("image_load: detected PNG by magic\n");
        ok = image_load_png(img, data, size);
    }
    else if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
        serial_printf("image_load: detected BMP by magic\n");
        ok = image_load_bmp(img, data, size);
    }
    else if (name && ends_with(name, ".bmp"))
        ok = image_load_bmp(img, data, size);
    else if (name && ends_with(name, ".tga"))
        ok = image_load_tga(img, data, size);
    else if (name && ends_with(name, ".png"))
        ok = image_load_png(img, data, size);
    else
        ok = image_load_tga(img, data, size);

    serial_printf("image_load: result=%s valid=%s %dx%d\n",
                  ok ? "OK" : "FAIL", img->valid ? "yes" : "no",
                  img->width, img->height);

    /* Copy to a pool slot so multiple images can coexist */
    if (ok && img->valid && img->pixels == img_pixels) {
        int npx = img->width * img->height;
        if (npx <= IMG_SLOT_SIZE) {
            uint32_t* slot = img_alloc_pixels();
            memcpy(slot, img_pixels, npx * sizeof(uint32_t));
            img->pixels = slot;
        }
        /* If too large for a slot, it stays in img_pixels (only 1 at a time) */
    }
    return ok;
}

/* ====== TEST IMAGE GENERATOR ====== */
/* Creates small test images in ramfs (must fit in RAMFS_MAX_DATA = 4096) */
void image_create_test(void) {
    /* 32x32 24-bit BMP = 54 + 32*96 = 3126 bytes */
    int w = 32, h = 32;
    int row_bytes = w * 3;  /* Already aligned to 4 */
    int img_size = row_bytes * h;
    int file_size = 54 + img_size;
    static uint8_t bmp[4096];

    memset(bmp, 0, sizeof(bmp));
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = file_size; bmp[3] = file_size >> 8;
    bmp[4] = file_size >> 16; bmp[5] = file_size >> 24;
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = w; bmp[22] = h;
    bmp[26] = 1; bmp[28] = 24;

    for (int y = 0; y < h; y++) {
        uint8_t* row = bmp + 54 + y * row_bytes;
        for (int x = 0; x < w; x++) {
            row[x*3 + 0] = (x + y) * 255 / (w + h);
            row[x*3 + 1] = y * 255 / h;
            row[x*3 + 2] = x * 255 / w;
        }
    }
    ramfs_write("/home/user/gradient.bmp", (const char*)bmp, file_size);

    /* 24x24 TGA = 18 + 24*24*3 = 1746 bytes */
    static uint8_t tga[2048];
    int tw = 24, th = 24;
    memset(tga, 0, 18);
    tga[2] = 2; tga[12] = tw; tga[14] = th; tga[16] = 24; tga[17] = 0x20;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int off = 18 + (y * tw + x) * 3;
            int v1 = (x * 11 + y * 7) & 0xFF;
            int v2 = (x * 5 - y * 9) & 0xFF;
            tga[off] = v1; tga[off+1] = (v1+v2)/2; tga[off+2] = v2;
        }
    ramfs_write("/home/user/plasma.tga", (const char*)tga, 18 + tw*th*3);
}
