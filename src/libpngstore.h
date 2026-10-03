#ifndef LIBPNGSTORE_H_
#define LIBPNGSTORE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cum.h"

#define PUBLIC
#define PRIVATE static

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef enum {
        PNG_COLOR_GRAY       = 0,
        PNG_COLOR_RGB        = 2,
        PNG_COLOR_PALETTE    = 3,
        PNG_COLOR_GRAY_ALPHA = 4,
        PNG_COLOR_RGBA       = 6,
} PNG_Color_Type;

typedef struct IHDR IHDR;
struct IHDR {
        u32 width;
        u32 height;
        u8 bit_depth;
        u8 color_type;
        u8 compression;
        u8 filter;
        u8 interlace;
};

typedef struct RGB RGB;
struct RGB {
        u8 r, g, b;
};
_Static_assert(sizeof(RGB) == 3, "RGB must match the PLTE on-disk layout");

typedef struct PLTE PLTE;
struct PLTE {
        u32 count;
        RGB *entries;
};

typedef enum {
        CHUNK_RAW,
        CHUNK_IHDR,
        CHUNK_PLTE,
        CHUNK_IEND,
} Chunk_Kind;

typedef struct Chunk Chunk;
struct Chunk {
        u32 len;
        u32 type;
        u32 CRC;

        u8 critical     : 1;
        u8 public       : 1;
        u8 future       : 1;
        u8 safe_to_copy : 1;

        Chunk_Kind kind;
        union {
                u8 *data;
                IHDR ihdr;
                PLTE plte;
        };
};

typedef struct Image Image;
struct Image {
        u32 width;
        u32 height;
        u8 color_type;
        u8 bit_depth;
        u8 channels;
        size_t stride;
        u8 *data;
};

typedef struct PNG PNG;
struct PNG {
        Da(Chunk) chunks;
        enum {
                PNG_ERR_OK,
                PNG_ERR_BAD_SIGNATURE,
                PNG_ERR_OPEN,
                PNG_ERR_READ,
                PNG_ERR_TRUNCATED,
                PNG_ERR_CHUNK_TOO_LARGE,
                PNG_ERR_NOMEM,
                PNG_ERR_BAD_CRC,
                PNG_ERR_MISSING_IEND,
                PNG_ERR_MISSING_IHDR,
                PNG_ERR_BAD_CHUNK,
                PNG_ERR_BAD_IHDR,
                PNG_ERR_BAD_IDAT,
                PNG_ERR_IMAGE_MISMATCH,
                PNG_ERR_ZLIB,
                PNG_ERR_BAD_NAME,
                PNG_ERR_NOT_FOUND,
                PNG_ERR_NO_SPACE,
                PNG_ERR_UNSUPPORTED,
        } err;
};

static const u8 PNG_SIGNATURE[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

PUBLIC void chunk_print(Chunk c);
PUBLIC void ihdr_print(IHDR h);
PUBLIC void plte_print(PLTE p);
PUBLIC PNG png_read(FILE *f);
PUBLIC int png_write(PNG png, FILE *dest);
PUBLIC Chunk *png_find(PNG png, Chunk_Kind kind);
PUBLIC void png_free(PNG png);
PUBLIC char *png_strerror(PNG png);

PUBLIC int png_get_image(PNG *png, Image *img);
PUBLIC int png_set_image(PNG *png, Image img);
PUBLIC u16 image_get(Image img, u32 x, u32 y, u32 channel);
PUBLIC void image_set(Image img, u32 x, u32 y, u32 channel, u16 value);
PUBLIC void image_free(Image img);

PUBLIC int png_add_section(PNG *png, const char *name, const u8 *data, size_t len);
PUBLIC int png_get_section(PNG *png, const char *name, u8 **data, size_t *len);
PUBLIC size_t png_embed_capacity(PNG *png);
PUBLIC int png_embed(PNG *png, const u8 *data, size_t len);
PUBLIC int png_extract(PNG *png, u8 **data, size_t *len);

#endif
