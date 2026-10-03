#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "cum.h"
#include "libpngstore.h"

PRIVATE u32
read_be32(const u8 *p)
{
        return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | (u32) p[3];
}

PRIVATE void
write_be32(u8 *p, u32 v)
{
        p[0] = v >> 24;
        p[1] = v >> 16;
        p[2] = v >> 8;
        p[3] = v;
}

PUBLIC void
chunk_print(Chunk c)
{
        printf("[CHUNK] type: %.4x (%4.4s) len: %u %s%s%s\n",
               c.type, (char *) &c.type, c.len,
               c.public ? "public " : "private ",
               c.critical ? "critical " : "ancillary ",
               c.safe_to_copy ? "copy safe " : "copy unsafe ");
}

PUBLIC void
ihdr_print(IHDR h)
{
        printf("[IHDR] %ux%u bit_depth: %u color_type: %u compression: %u filter: %u interlace: %u\n",
               h.width, h.height, h.bit_depth, h.color_type,
               h.compression, h.filter, h.interlace);
}

PUBLIC void
plte_print(PLTE p)
{
        printf("[PLTE] %u entries\n", p.count);
        for (u32 i = 0; i < p.count; i++) {
                printf("  %3u: #%02x%02x%02x\n", i,
                       p.entries[i].r, p.entries[i].g, p.entries[i].b);
        }
}

PRIVATE PNG
png_new()
{
        return (PNG) { 0 };
}

PRIVATE u32
crc_update(u32 crc, const u8 *buf, size_t len)
{
        static u32 table[256];
        static int table_ready = 0;

        if (!table_ready) {
                for (u32 n = 0; n < 256; n++) {
                        u32 c = n;
                        for (int k = 0; k < 8; k++) {
                                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
                        }
                        table[n] = c;
                }
                table_ready = 1;
        }

        for (size_t i = 0; i < len; i++) {
                crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
        }
        return crc;
}

PRIVATE u32
chunk_crc(u32 type, const u8 *data, u32 len)
{
        u32 crc = 0xffffffffu;
        crc     = crc_update(crc, (const u8 *) &type, sizeof type);
        crc     = crc_update(crc, data, len);
        return crc ^ 0xffffffffu;
}

PRIVATE int
crc_verify(Chunk c)
{
        return chunk_crc(c.type, c.data, c.len) != c.CRC;
}

PRIVATE void
chunk_set_flags(Chunk *c)
{
        c->critical     = isupper(((char *) &c->type)[0]) ? 1u : 0u;
        c->public       = isupper(((char *) &c->type)[1]) ? 1u : 0u;
        c->future       = isupper(((char *) &c->type)[2]) ? 1u : 0u;
        c->safe_to_copy = islower(((char *) &c->type)[3]) ? 1u : 0u;
}

PRIVATE int
png_fread(void *buf, size_t n, FILE *f, PNG *png)
{
        if (fread(buf, 1, n, f) == n) return 0;
        png->err = ferror(f) ? PNG_ERR_READ : PNG_ERR_TRUNCATED;
        return 1;
}

PRIVATE int
chunk_is(Chunk c, const char *type)
{
        return memcmp(&c.type, type, sizeof c.type) == 0;
}

PRIVATE void
chunk_free(Chunk c)
{
        switch (c.kind) {
        case CHUNK_RAW: free(c.data); break;
        case CHUNK_PLTE: free(c.plte.entries); break;
        case CHUNK_IHDR:
        case CHUNK_IEND: break;
        }
}

PRIVATE int
chunk_decode(Chunk *c, PNG *png)
{
        u8 *data = c->data;

        if (chunk_is(*c, "IHDR")) {
                if (c->len != 13) goto bad;
                IHDR *h        = &c->ihdr;
                h->width       = read_be32(data);
                h->height      = read_be32(data + 4);
                h->bit_depth   = data[8];
                h->color_type  = data[9];
                h->compression = data[10];
                h->filter      = data[11];
                h->interlace   = data[12];
                c->kind        = CHUNK_IHDR;
                free(data);
        } else if (chunk_is(*c, "PLTE")) {
                if (c->len == 0 || c->len % 3 || c->len / 3 > 256) goto bad;
                c->plte.count   = c->len / 3;
                c->plte.entries = (RGB *) data;
                c->kind         = CHUNK_PLTE;
        } else if (chunk_is(*c, "IEND")) {
                if (c->len != 0) goto bad;
                c->kind = CHUNK_IEND;
                free(data);
        }
        return 0;

bad:
        png->err = PNG_ERR_BAD_CHUNK;
        free(data);
        return 1;
}

PRIVATE int
png_read_chunk(FILE *f, PNG *png)
{
        Chunk chunk = { .kind = CHUNK_RAW };

        int c = fgetc(f);
        if (c == EOF) {
                png->err = ferror(f) ? PNG_ERR_READ : PNG_ERR_MISSING_IEND;
                return 1;
        }
        ungetc(c, f);

        u8 be[4];
        if (png_fread(be, sizeof be, f, png)) return 1;
        chunk.len = read_be32(be);
        if (png_fread(&chunk.type, sizeof chunk.type, f, png)) return 1;

        if (chunk.len > 0x7fffffffu) {
                png->err = PNG_ERR_CHUNK_TOO_LARGE;
                return 1;
        }

        if (chunk.len > 0) {
                chunk.data = malloc(chunk.len);
                if (chunk.data == NULL) {
                        png->err = PNG_ERR_NOMEM;
                        return 1;
                }
                if (png_fread(chunk.data, chunk.len, f, png)) goto fail;
        }
        if (png_fread(be, sizeof be, f, png)) goto fail;
        chunk.CRC = read_be32(be);
        if (crc_verify(chunk)) {
                png->err = PNG_ERR_BAD_CRC;
                goto fail;
        }

        chunk_set_flags(&chunk);

        if (chunk_decode(&chunk, png)) return 1;

        Da_append(&png->chunks, chunk);
        return 0;

fail:
        free(chunk.data);
        return 1;
}

PRIVATE int
png_read_header(FILE *f, PNG *png)
{
        u8 sig[sizeof PNG_SIGNATURE];
        size_t n = fread(sig, 1, sizeof sig, f);
        if (n != sizeof sig || memcmp(sig, PNG_SIGNATURE, sizeof sig) != 0) {
                png->err = PNG_ERR_BAD_SIGNATURE;
                return 1;
        }

        return 0;
}

PUBLIC PNG
png_read(FILE *f)
{
        PNG png = png_new();
        if (f == NULL) {
                png.err = PNG_ERR_OPEN;
                return png;
        }
        if (png_read_header(f, &png)) return png;

        if (png_read_chunk(f, &png)) return png;
        if (png.chunks.items[0].kind != CHUNK_IHDR) {
                png.err = PNG_ERR_MISSING_IHDR;
                return png;
        }

        for (;;) {
                if (png_read_chunk(f, &png)) return png;
                if (png.chunks.items[png.chunks.count - 1].kind == CHUNK_IEND) break;
        }

        return png;
}

PRIVATE int
chunk_write(Chunk c, FILE *dest)
{
        u8 ihdr[13];
        const u8 *data = NULL;
        u32 len        = 0;

        switch (c.kind) {
        case CHUNK_RAW:
                data = c.data;
                len  = c.len;
                break;
        case CHUNK_IHDR:
                write_be32(ihdr, c.ihdr.width);
                write_be32(ihdr + 4, c.ihdr.height);
                ihdr[8]  = c.ihdr.bit_depth;
                ihdr[9]  = c.ihdr.color_type;
                ihdr[10] = c.ihdr.compression;
                ihdr[11] = c.ihdr.filter;
                ihdr[12] = c.ihdr.interlace;
                data     = ihdr;
                len      = sizeof ihdr;
                break;
        case CHUNK_PLTE:
                data = (const u8 *) c.plte.entries;
                len  = c.plte.count * sizeof(RGB);
                break;
        case CHUNK_IEND: break;
        }

        u32 crc = chunk_crc(c.type, data, len);

        u8 be[4];
        write_be32(be, len);
        if (fwrite(be, 1, sizeof be, dest) != sizeof be) return 1;
        if (fwrite(&c.type, 1, sizeof c.type, dest) != sizeof c.type) return 1;
        if (len && fwrite(data, 1, len, dest) != len) return 1;
        write_be32(be, crc);
        if (fwrite(be, 1, sizeof be, dest) != sizeof be) return 1;
        return 0;
}

PUBLIC int
png_write(PNG png, FILE *dest)
{
        if (dest == NULL) return 1;
        if (fwrite(PNG_SIGNATURE, 1, sizeof PNG_SIGNATURE, dest) != sizeof PNG_SIGNATURE) {
                return 1;
        }
        Da_foreach(c, png.chunks)
        {
                if (chunk_write(*c, dest)) return 1;
        }
        return fflush(dest) != 0;
}

PUBLIC Chunk *
png_find(PNG png, Chunk_Kind kind)
{
        Da_foreach(c, png.chunks)
        {
                if (c->kind == kind) return c;
        }
        return NULL;
}

PUBLIC void
png_free(PNG png)
{
        Da_foreach(c, png.chunks)
        {
                chunk_free(*c);
        }
        Da_destroy(&png.chunks);
}

PUBLIC char *
png_strerror(PNG png)
{
        static char *const lut[] = {
                [PNG_ERR_OK]              = "success",
                [PNG_ERR_BAD_SIGNATURE]   = "not a png file (bad signature)",
                [PNG_ERR_OPEN]            = "cannot open file",
                [PNG_ERR_READ]            = "read error",
                [PNG_ERR_TRUNCATED]       = "unexpected end of file",
                [PNG_ERR_CHUNK_TOO_LARGE] = "chunk length exceeds 2^31-1",
                [PNG_ERR_NOMEM]           = "out of memory",
                [PNG_ERR_BAD_CRC]         = "chunk CRC mismatch",
                [PNG_ERR_MISSING_IEND]    = "missing IEND chunk",
                [PNG_ERR_MISSING_IHDR]    = "first chunk is not IHDR",
                [PNG_ERR_BAD_CHUNK]       = "malformed chunk",
                [PNG_ERR_BAD_IHDR]        = "invalid or unsupported IHDR",
                [PNG_ERR_BAD_IDAT]        = "corrupt image data",
                [PNG_ERR_IMAGE_MISMATCH]  = "image does not match IHDR",
                [PNG_ERR_ZLIB]            = "zlib error",
                [PNG_ERR_BAD_NAME]        = "section name must be 4 ascii letters",
                [PNG_ERR_NOT_FOUND]       = "no stored data found",
                [PNG_ERR_NO_SPACE]        = "data does not fit in the image",
                [PNG_ERR_UNSUPPORTED]     = "unsupported image format",
        };
        if (png.err < 0 || png.err >= sizeof lut / sizeof *lut) {
                return "invalid error number";
        }
        return lut[png.err];
}

#define IDAT_MAX (1u << 20)

typedef struct Pass Pass;
struct Pass {
        u8 x0, y0, dx, dy;
};

static const Pass ADAM7[7] = {
        { 0, 0, 8, 8 },
        { 4, 0, 8, 8 },
        { 0, 4, 4, 8 },
        { 2, 0, 4, 4 },
        { 0, 2, 2, 4 },
        { 1, 0, 2, 2 },
        { 0, 1, 1, 2 },
};
static const Pass NO_INTERLACE[1] = { { 0, 0, 1, 1 } };

PRIVATE u8
png_channels(u8 color_type)
{
        switch (color_type) {
        case PNG_COLOR_GRAY:
        case PNG_COLOR_PALETTE: return 1;
        case PNG_COLOR_GRAY_ALPHA: return 2;
        case PNG_COLOR_RGB: return 3;
        case PNG_COLOR_RGBA: return 4;
        }
        return 0;
}

PRIVATE int
ihdr_valid(IHDR h)
{
        if (h.width == 0 || h.width > 0x7fffffffu) return 0;
        if (h.height == 0 || h.height > 0x7fffffffu) return 0;
        if (h.compression != 0 || h.filter != 0 || h.interlace > 1) return 0;

        u8 d = h.bit_depth;
        switch (h.color_type) {
        case PNG_COLOR_GRAY: return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
        case PNG_COLOR_PALETTE: return d == 1 || d == 2 || d == 4 || d == 8;
        case PNG_COLOR_RGB:
        case PNG_COLOR_GRAY_ALPHA:
        case PNG_COLOR_RGBA: return d == 8 || d == 16;
        }
        return 0;
}

PRIVATE u64
row_bytes(u32 width, u32 bits_per_pixel)
{
        return ((u64) width * bits_per_pixel + 7) / 8;
}

PRIVATE u32
pass_size(u32 n, u8 start, u8 step)
{
        return n > start ? (n - start + step - 1) / step : 0;
}

PRIVATE int
png_passes(IHDR h, const Pass **passes)
{
        *passes = h.interlace ? ADAM7 : NO_INTERLACE;
        return h.interlace ? 7 : 1;
}

PRIVATE size_t
raw_size(IHDR h, u32 bits)
{
        const Pass *passes;
        int npasses = png_passes(h, &passes);
        u64 size    = 0;
        for (int i = 0; i < npasses; i++) {
                u32 pw = pass_size(h.width, passes[i].x0, passes[i].dx);
                u32 ph = pass_size(h.height, passes[i].y0, passes[i].dy);
                if (pw == 0 || ph == 0) continue;
                size += (u64) ph * (1 + row_bytes(pw, bits));
        }
        return size > SIZE_MAX ? 0 : (size_t) size;
}

PRIVATE int
image_init(Image *img, IHDR h)
{
        *img = (Image) {
                .width      = h.width,
                .height     = h.height,
                .color_type = h.color_type,
                .bit_depth  = h.bit_depth,
                .channels   = png_channels(h.color_type),
        };
        u64 stride = row_bytes(h.width, img->channels * h.bit_depth);
        if (stride > SIZE_MAX / h.height) return 1;
        img->stride = stride;
        return 0;
}

PRIVATE int
image_matches(Image img, IHDR h)
{
        Image want;
        if (image_init(&want, h)) return 0;
        return img.data && img.width == want.width && img.height == want.height &&
               img.color_type == want.color_type && img.bit_depth == want.bit_depth &&
               img.channels == want.channels && img.stride == want.stride;
}

PRIVATE void
pixel_copy(u8 *dst, u64 dx, const u8 *src, u64 sx, u32 bits)
{
        if (bits >= 8) {
                memcpy(dst + dx * (bits / 8), src + sx * (bits / 8), bits / 8);
                return;
        }
        u8 mask       = (1u << bits) - 1;
        u64 sbit      = sx * bits;
        u64 dbit      = dx * bits;
        u8 v          = (src[sbit / 8] >> (8 - bits - sbit % 8)) & mask;
        u8 shift      = 8 - bits - dbit % 8;
        dst[dbit / 8] = (dst[dbit / 8] & ~(mask << shift)) | (v << shift);
}

PRIVATE void
pass_row_copy(Image img, Pass p, u32 py, u8 *row, u32 pw, int to_image)
{
        u32 bits   = img.channels * img.bit_depth;
        u8 *imgrow = img.data + (size_t) (p.y0 + (u64) py * p.dy) * img.stride;
        if (p.dx == 1) {
                if (to_image)
                        memcpy(imgrow, row, img.stride);
                else
                        memcpy(row, imgrow, img.stride);
                return;
        }
        for (u32 x = 0; x < pw; x++) {
                u64 ix = p.x0 + (u64) x * p.dx;
                if (to_image)
                        pixel_copy(imgrow, ix, row, x, bits);
                else
                        pixel_copy(row, x, imgrow, ix, bits);
        }
}

PRIVATE u8
paeth(u8 a, u8 b, u8 c)
{
        int p  = a + b - c;
        int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        return pb <= pc ? b : c;
}

PRIVATE u8
predict(u8 type, u8 a, u8 b, u8 c)
{
        switch (type) {
        case 1: return a;
        case 2: return b;
        case 3: return (a + b) / 2;
        case 4: return paeth(a, b, c);
        default: return 0;
        }
}

PRIVATE void
unfilter(u8 type, u8 *row, const u8 *prev, size_t len, size_t bpp)
{
        for (size_t i = 0; i < len; i++) {
                u8 a = i >= bpp ? row[i - bpp] : 0;
                u8 b = prev ? prev[i] : 0;
                u8 c = prev && i >= bpp ? prev[i - bpp] : 0;
                row[i] += predict(type, a, b, c);
        }
}

PRIVATE void
filter(u8 type, u8 *out, const u8 *row, const u8 *prev, size_t len, size_t bpp)
{
        for (size_t i = 0; i < len; i++) {
                u8 a   = i >= bpp ? row[i - bpp] : 0;
                u8 b   = prev ? prev[i] : 0;
                u8 c   = prev && i >= bpp ? prev[i - bpp] : 0;
                out[i] = row[i] - predict(type, a, b, c);
        }
}

PRIVATE int
idat_inflate(PNG *png, u8 *out, size_t size)
{
        z_stream zs = { 0 };
        if (inflateInit(&zs) != Z_OK) {
                png->err = PNG_ERR_ZLIB;
                return 1;
        }

        size_t done = 0;
        int ret     = Z_OK;
        Da_foreach(c, png->chunks)
        {
                if (c->kind != CHUNK_RAW || !chunk_is(*c, "IDAT")) continue;
                zs.next_in  = c->data;
                zs.avail_in = c->len;
                while (done < size && ret != Z_STREAM_END && (zs.avail_in || zs.avail_out == 0)) {
                        size_t room  = size - done;
                        zs.next_out  = out + done;
                        zs.avail_out = room > UINT_MAX ? UINT_MAX : (uInt) room;
                        uInt before  = zs.avail_out;
                        ret          = inflate(&zs, Z_NO_FLUSH);
                        done += before - zs.avail_out;
                        if (ret != Z_OK && ret != Z_STREAM_END) {
                                inflateEnd(&zs);
                                png->err = PNG_ERR_BAD_IDAT;
                                return 1;
                        }
                }
                if (done == size || ret == Z_STREAM_END) break;
        }
        inflateEnd(&zs);

        if (done != size) {
                png->err = PNG_ERR_BAD_IDAT;
                return 1;
        }
        return 0;
}

PUBLIC int
png_get_image(PNG *png, Image *img)
{
        Chunk *hc = png_find(*png, CHUNK_IHDR);
        if (hc == NULL) {
                png->err = PNG_ERR_MISSING_IHDR;
                return 1;
        }
        IHDR h = hc->ihdr;
        if (!ihdr_valid(h) || image_init(img, h)) {
                png->err = PNG_ERR_BAD_IHDR;
                return 1;
        }

        u32 bits    = img->channels * h.bit_depth;
        size_t bpp  = bits < 8 ? 1 : bits / 8;
        size_t size = raw_size(h, bits);
        if (size == 0) {
                png->err = PNG_ERR_NOMEM;
                return 1;
        }

        u8 *raw   = malloc(size);
        img->data = calloc(img->height, img->stride);
        if (raw == NULL || img->data == NULL) {
                png->err = PNG_ERR_NOMEM;
                goto fail;
        }
        if (idat_inflate(png, raw, size)) goto fail;

        const Pass *passes;
        int npasses = png_passes(h, &passes);
        u8 *p       = raw;
        for (int i = 0; i < npasses; i++) {
                u32 pw = pass_size(h.width, passes[i].x0, passes[i].dx);
                u32 ph = pass_size(h.height, passes[i].y0, passes[i].dy);
                if (pw == 0 || ph == 0) continue;

                size_t len = row_bytes(pw, bits);
                u8 *prev   = NULL;
                for (u32 y = 0; y < ph; y++, p += 1 + len) {
                        u8 type = p[0];
                        u8 *row = p + 1;
                        if (type > 4) {
                                png->err = PNG_ERR_BAD_IDAT;
                                goto fail;
                        }
                        unfilter(type, row, prev, len, bpp);
                        pass_row_copy(*img, passes[i], y, row, pw, 1);
                        prev = row;
                }
        }

        free(raw);
        return 0;

fail:
        free(raw);
        free(img->data);
        img->data = NULL;
        return 1;
}

PRIVATE u8
filter_best(u8 *out, u8 *tmp, const u8 *row, const u8 *prev, size_t len, size_t bpp)
{
        u8 best      = 0;
        u64 best_sum = UINT64_MAX;
        for (u8 type = 0; type <= 4; type++) {
                filter(type, tmp, row, prev, len, bpp);
                u64 sum = 0;
                for (size_t i = 0; i < len; i++)
                        sum += abs((int8_t) tmp[i]);
                if (sum < best_sum) {
                        best     = type;
                        best_sum = sum;
                        memcpy(out, tmp, len);
                }
        }
        return best;
}

typedef Da(Chunk) Chunks;

PRIVATE int
idat_deflate(const u8 *raw, size_t size, Chunks *out)
{
        z_stream zs = { 0 };
        if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) return 1;

        size_t fed = 0;
        int ret    = Z_OK;
        while (ret != Z_STREAM_END) {
                u8 *buf = malloc(IDAT_MAX);
                if (buf == NULL) goto fail;
                zs.next_out  = buf;
                zs.avail_out = IDAT_MAX;

                while (zs.avail_out && ret != Z_STREAM_END) {
                        if (zs.avail_in == 0) {
                                size_t left = size - fed;
                                zs.next_in  = (u8 *) raw + fed;
                                zs.avail_in = left > UINT_MAX ? UINT_MAX : (uInt) left;
                                fed += zs.avail_in;
                        }
                        ret = deflate(&zs, fed == size ? Z_FINISH : Z_NO_FLUSH);
                        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                                free(buf);
                                goto fail;
                        }
                }

                u32 len = IDAT_MAX - zs.avail_out;
                Chunk c = { .kind = CHUNK_RAW, .len = len, .data = buf };
                memcpy(&c.type, "IDAT", sizeof c.type);
                chunk_set_flags(&c);
                c.CRC = chunk_crc(c.type, c.data, c.len);
                Da_append(out, c);
        }

        deflateEnd(&zs);
        return 0;

fail:
        deflateEnd(&zs);
        return 1;
}

PUBLIC int
png_set_image(PNG *png, Image img)
{
        Chunk *hc = png_find(*png, CHUNK_IHDR);
        if (hc == NULL) {
                png->err = PNG_ERR_MISSING_IHDR;
                return 1;
        }
        IHDR h = hc->ihdr;
        if (!ihdr_valid(h)) {
                png->err = PNG_ERR_BAD_IHDR;
                return 1;
        }
        if (!image_matches(img, h)) {
                png->err = PNG_ERR_IMAGE_MISMATCH;
                return 1;
        }

        u32 bits    = img.channels * img.bit_depth;
        size_t bpp  = bits < 8 ? 1 : bits / 8;
        size_t size = raw_size(h, bits);
        if (size == 0) {
                png->err = PNG_ERR_NOMEM;
                return 1;
        }

        int adaptive = img.color_type != PNG_COLOR_PALETTE && img.bit_depth >= 8;

        Chunks idat = { 0 };
        u8 *raw     = malloc(size);
        u8 *rows    = malloc(3 * img.stride);
        if (raw == NULL || rows == NULL) {
                png->err = PNG_ERR_NOMEM;
                goto fail;
        }

        const Pass *passes;
        int npasses = png_passes(h, &passes);
        u8 *p       = raw;
        for (int i = 0; i < npasses; i++) {
                u32 pw = pass_size(h.width, passes[i].x0, passes[i].dx);
                u32 ph = pass_size(h.height, passes[i].y0, passes[i].dy);
                if (pw == 0 || ph == 0) continue;

                size_t len = row_bytes(pw, bits);
                u8 *prev   = NULL;
                u8 *cur    = rows + img.stride;
                for (u32 y = 0; y < ph; y++, p += 1 + len) {
                        memset(cur, 0, len);
                        pass_row_copy(img, passes[i], y, cur, pw, 0);
                        if (adaptive) {
                                p[0] = filter_best(p + 1, rows + 2 * img.stride, cur, prev, len, bpp);
                        } else {
                                p[0] = 0;
                                memcpy(p + 1, cur, len);
                        }

                        prev = cur;
                        cur  = cur == rows ? rows + img.stride : rows;
                }
        }

        if (idat_deflate(raw, size, &idat)) {
                png->err = PNG_ERR_ZLIB;
                goto fail;
        }

        int at = -1;
        for (int i = 0; i < png->chunks.count;) {
                Chunk *c = &png->chunks.items[i];
                if (c->kind == CHUNK_RAW && chunk_is(*c, "IDAT")) {
                        if (at < 0) at = i;
                        chunk_free(*c);
                        Da_remove(&png->chunks, i);
                } else {
                        if (at < 0 && c->kind == CHUNK_IEND) at = i;
                        i++;
                }
        }
        if (at < 0) at = png->chunks.count;
        for (int i = 0; i < idat.count; i++) {
                Da_insert(&png->chunks, idat.items[i], at + i);
        }

        Da_destroy(&idat);
        free(rows);
        free(raw);
        return 0;

fail:
        Da_foreach(c, idat) chunk_free(*c);
        Da_destroy(&idat);
        free(rows);
        free(raw);
        return 1;
}

PUBLIC u16
image_get(Image img, u32 x, u32 y, u32 channel)
{
        const u8 *row = img.data + (size_t) y * img.stride;
        u64 i         = (u64) x * img.channels + channel;
        switch (img.bit_depth) {
        case 16: return (u16) (row[2 * i] << 8 | row[2 * i + 1]);
        case 8: return row[i];
        default: {
                u64 bit = i * img.bit_depth;
                u8 mask = (1u << img.bit_depth) - 1;
                return (row[bit / 8] >> (8 - img.bit_depth - bit % 8)) & mask;
        }
        }
}

PUBLIC void
image_set(Image img, u32 x, u32 y, u32 channel, u16 value)
{
        u8 *row = img.data + (size_t) y * img.stride;
        u64 i   = (u64) x * img.channels + channel;
        switch (img.bit_depth) {
        case 16:
                row[2 * i]     = value >> 8;
                row[2 * i + 1] = value;
                break;
        case 8: row[i] = value; break;
        default: {
                u64 bit      = i * img.bit_depth;
                u8 mask      = (1u << img.bit_depth) - 1;
                u8 shift     = 8 - img.bit_depth - bit % 8;
                row[bit / 8] = (row[bit / 8] & ~(mask << shift)) | ((value & mask) << shift);
        }
        }
}

PUBLIC void
image_free(Image img)
{
        free(img.data);
}

PRIVATE int
section_type(const char *name, u32 *type)
{
        if (name == NULL || strlen(name) != 4) return 1;
        char t[4];
        for (int i = 0; i < 4; i++) {
                char c = name[i] | 0x20;
                if (c < 'a' || c > 'z') return 1;
                t[i] = i == 2 ? c & ~0x20 : c;
        }
        memcpy(type, t, sizeof *type);
        return 0;
}

PUBLIC int
png_add_section(PNG *png, const char *name, const u8 *data, size_t len)
{
        Chunk c = { .kind = CHUNK_RAW };
        if (section_type(name, &c.type)) {
                png->err = PNG_ERR_BAD_NAME;
                return 1;
        }
        if (len > 0x7fffffffu) {
                png->err = PNG_ERR_CHUNK_TOO_LARGE;
                return 1;
        }
        Chunk *iend = png_find(*png, CHUNK_IEND);
        if (iend == NULL) {
                png->err = PNG_ERR_MISSING_IEND;
                return 1;
        }

        c.len = len;
        if (len) {
                c.data = malloc(len);
                if (c.data == NULL) {
                        png->err = PNG_ERR_NOMEM;
                        return 1;
                }
                memcpy(c.data, data, len);
        }
        chunk_set_flags(&c);
        c.CRC = chunk_crc(c.type, c.data, c.len);
        Da_insert(&png->chunks, c, Da_index(iend, &png->chunks));
        return 0;
}

PUBLIC int
png_get_section(PNG *png, const char *name, u8 **data, size_t *len)
{
        u32 type;
        if (section_type(name, &type)) {
                png->err = PNG_ERR_BAD_NAME;
                return 1;
        }
        Da_foreach(c, png->chunks)
        {
                if (c->kind != CHUNK_RAW || c->type != type) continue;
                *data = malloc(c->len ? c->len : 1);
                if (*data == NULL) {
                        png->err = PNG_ERR_NOMEM;
                        return 1;
                }
                if (c->len) memcpy(*data, c->data, c->len);
                *len = c->len;
                return 0;
        }
        png->err = PNG_ERR_NOT_FOUND;
        return 1;
}

#define LSB_BITS 2
#define LSB_MASK ((1u << LSB_BITS) - 1)
#define LSB_MAGIC "PNGs"
#define LSB_HEADER (sizeof LSB_MAGIC - 1 + 4)
_Static_assert(8 % LSB_BITS == 0, "a byte must span whole samples");

typedef struct Lsb Lsb;
struct Lsb {
        Image img;
        u64 i;
};

PRIVATE u64
lsb_samples(Image img)
{
        return (u64) img.width * img.height * img.channels;
}

PRIVATE int
lsb_open(PNG *png, Lsb *s)
{
        *s = (Lsb) { 0 };
        if (png_get_image(png, &s->img)) return 1;
        if (s->img.color_type == PNG_COLOR_PALETTE || s->img.bit_depth < 8) {
                image_free(s->img);
                png->err = PNG_ERR_UNSUPPORTED;
                return 1;
        }
        return 0;
}

PRIVATE void
lsb_put(Lsb *s, u8 byte)
{
        u8 ch = s->img.channels;
        for (int k = 8 - LSB_BITS; k >= 0; k -= LSB_BITS, s->i++) {
                u64 px = s->i / ch;
                u32 x  = px % s->img.width;
                u32 y  = px / s->img.width;
                u16 v  = image_get(s->img, x, y, s->i % ch);
                v      = (v & ~LSB_MASK) | ((byte >> k) & LSB_MASK);
                image_set(s->img, x, y, s->i % ch, v);
        }
}

PRIVATE u8
lsb_get(Lsb *s)
{
        u8 ch   = s->img.channels;
        u8 byte = 0;
        for (int k = 8 - LSB_BITS; k >= 0; k -= LSB_BITS, s->i++) {
                u64 px = s->i / ch;
                u16 v  = image_get(s->img, px % s->img.width, px / s->img.width, s->i % ch);
                byte |= (v & LSB_MASK) << k;
        }
        return byte;
}

PUBLIC size_t
png_embed_capacity(PNG *png)
{
        Lsb s;
        if (lsb_open(png, &s)) return 0;
        u64 bytes = lsb_samples(s.img) * LSB_BITS / 8;
        image_free(s.img);
        bytes = bytes > LSB_HEADER ? bytes - LSB_HEADER : 0;
        if (bytes > UINT32_MAX) bytes = UINT32_MAX;
        return bytes;
}

PUBLIC int
png_embed(PNG *png, const u8 *data, size_t len)
{
        Lsb s;
        if (lsb_open(png, &s)) return 1;
        u64 room = lsb_samples(s.img) * LSB_BITS / 8;
        if (len > UINT32_MAX || room < LSB_HEADER || len > room - LSB_HEADER) {
                image_free(s.img);
                png->err = PNG_ERR_NO_SPACE;
                return 1;
        }

        u8 be[4];
        write_be32(be, len);
        for (size_t i = 0; i < sizeof LSB_MAGIC - 1; i++)
                lsb_put(&s, LSB_MAGIC[i]);
        for (size_t i = 0; i < sizeof be; i++)
                lsb_put(&s, be[i]);
        for (size_t i = 0; i < len; i++)
                lsb_put(&s, data[i]);

        int ret = png_set_image(png, s.img);
        image_free(s.img);
        return ret;
}

PUBLIC int
png_extract(PNG *png, u8 **data, size_t *len)
{
        Lsb s;
        if (lsb_open(png, &s)) return 1;
        u64 room = lsb_samples(s.img) * LSB_BITS / 8;
        if (room < LSB_HEADER) goto not_found;

        u8 hdr[LSB_HEADER];
        for (size_t i = 0; i < sizeof hdr; i++)
                hdr[i] = lsb_get(&s);
        if (memcmp(hdr, LSB_MAGIC, sizeof LSB_MAGIC - 1) != 0) goto not_found;
        u32 n = read_be32(hdr + sizeof LSB_MAGIC - 1);
        if (n > room - LSB_HEADER) goto not_found;

        *data = malloc(n ? n : 1);
        if (*data == NULL) {
                image_free(s.img);
                png->err = PNG_ERR_NOMEM;
                return 1;
        }
        for (u32 i = 0; i < n; i++)
                (*data)[i] = lsb_get(&s);
        *len = n;
        image_free(s.img);
        return 0;

not_found:
        image_free(s.img);
        png->err = PNG_ERR_NOT_FOUND;
        return 1;
}
