#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flag.h"
#include "libpngstore.h"

typedef struct Opts Opts;
struct Opts {
        const char *verbose;
        const char *chunks;
        const char *capacity;
        const char *pixel;
        const char *set_pixel;
        const char *reencode;
        const char *embed;
        const char *extract;
        const char *section;
        const char *output;
};

PRIVATE int
load(const char *path, PNG *png)
{
        FILE *f = fopen(path, "rb");
        *png    = png_read(f);
        if (png->err == PNG_ERR_OPEN || png->err == PNG_ERR_READ) {
                fprintf(stderr, "Error: read png '%s': %s: %s\n",
                        path, png_strerror(*png), strerror(errno));
        } else if (png->err) {
                fprintf(stderr, "Error: read png '%s': %s\n",
                        path, png_strerror(*png));
        }
        if (f) fclose(f);
        if (png->err) png_free(*png);
        return png->err != PNG_ERR_OK;
}

PRIVATE int
save(PNG png, const char *path)
{
        FILE *f = fopen(path, "wb");
        int err = png_write(png, f);
        if (f && fclose(f)) err = 1;
        if (err) fprintf(stderr, "Error: write png '%s': %s\n", path, strerror(errno));
        return err;
}

PRIVATE int
read_file(const char *path, u8 **data, size_t *len)
{
        FILE *f = fopen(path, "rb");
        if (f == NULL) goto fail;

        size_t cap = 4096, n = 0;
        u8 *buf = malloc(cap);
        while (buf) {
                n += fread(buf + n, 1, cap - n, f);
                if (n < cap) break;
                u8 *tmp = realloc(buf, cap *= 2);
                if (tmp == NULL) free(buf);
                buf = tmp;
        }
        if (buf == NULL || ferror(f)) {
                free(buf);
                fclose(f);
                goto fail;
        }
        fclose(f);
        *data = buf;
        *len  = n;
        return 0;

fail:
        fprintf(stderr, "Error: read '%s': %s\n", path, strerror(errno));
        return 1;
}

PRIVATE int
write_file(const char *path, const u8 *data, size_t len)
{
        FILE *f = fopen(path, "wb");
        if (f && fwrite(data, 1, len, f) == len && fclose(f) == 0) return 0;
        fprintf(stderr, "Error: write '%s': %s\n", path, strerror(errno));
        if (f) fclose(f);
        return 1;
}

PRIVATE int
parse_u32(const char **s, u32 *v, char end)
{
        char *e;
        errno           = 0;
        unsigned long n = strtoul(*s, &e, 10);
        if (e == *s || *e != end || errno || n > UINT32_MAX || **s == '-') return 1;
        *v = n;
        *s = end ? e + 1 : e;
        return 0;
}

PRIVATE int
parse_xy(const char **s, u32 *x, u32 *y, char end)
{
        return parse_u32(s, x, ',') || parse_u32(s, y, end);
}

PRIVATE int
get_image(PNG *png, const char *path, Image *img)
{
        if (png_get_image(png, img) == 0) return 0;
        fprintf(stderr, "Error: decode '%s': %s\n", path, png_strerror(*png));
        return 1;
}

PRIVATE int
in_bounds(Image img, u32 x, u32 y)
{
        if (x < img.width && y < img.height) return 1;
        fprintf(stderr, "Error: pixel %u,%u outside of %ux%u image\n", x, y, img.width, img.height);
        return 0;
}

PRIVATE int
show_pixel(PNG *png, const char *path, const char *arg)
{
        u32 x, y;
        if (parse_xy(&arg, &x, &y, 0)) {
                fprintf(stderr, "Error: --pixel expects X,Y\n");
                return 1;
        }
        Image img;
        if (get_image(png, path, &img)) return 1;
        int ok = in_bounds(img, x, y);
        if (ok) {
                printf("[PIXEL] %u,%u:", x, y);
                for (u32 c = 0; c < img.channels; c++)
                        printf(" %u", image_get(img, x, y, c));
                printf("\n");
        }
        image_free(img);
        return !ok;
}

PRIVATE int
show(const char *path, Opts o)
{
        PNG png;
        if (load(path, &png)) return 1;

        int status = 0;
        int any    = o.chunks || o.capacity || o.pixel;
        if (!any) {
                ihdr_print(png_find(png, CHUNK_IHDR)->ihdr);
                Chunk *plte = png_find(png, CHUNK_PLTE);
                if (plte) plte_print(plte->plte);
        }
        if (o.chunks) {
                Da_foreach(c, png.chunks) chunk_print(*c);
        }
        if (o.capacity) {
                size_t cap = png_embed_capacity(&png);
                if (png.err) {
                        fprintf(stderr, "Error: capacity of '%s': %s\n", path, png_strerror(png));
                        status = 1;
                } else {
                        printf("[CAPACITY] %zu bytes\n", cap);
                }
        }
        if (o.pixel && show_pixel(&png, path, o.pixel)) status = 1;

        png_free(png);
        return status;
}

PRIVATE int
set_pixel(PNG *png, const char *path, const char *arg)
{
        u32 x, y;
        if (parse_xy(&arg, &x, &y, '=')) goto usage;

        Image img;
        if (get_image(png, path, &img)) return 1;
        if (!in_bounds(img, x, y)) goto fail;

        u32 max = (1u << img.bit_depth) - 1;
        for (u32 c = 0; c < img.channels; c++) {
                u32 v;
                if (parse_u32(&arg, &v, c + 1 < img.channels ? ',' : 0)) {
                        fprintf(stderr, "Error: --set-pixel needs %u comma separated values\n",
                                img.channels);
                        goto fail;
                }
                if (v > max) {
                        fprintf(stderr, "Error: value %u exceeds %u bits\n", v, img.bit_depth);
                        goto fail;
                }
                image_set(img, x, y, c, v);
        }

        int err = png_set_image(png, img);
        if (err) fprintf(stderr, "Error: encode '%s': %s\n", path, png_strerror(*png));
        image_free(img);
        return err;

fail:
        image_free(img);
        return 1;
usage:
        fprintf(stderr, "Error: --set-pixel expects X,Y=V[,V...]\n");
        return 1;
}

PRIVATE int
reencode(PNG *png, const char *path)
{
        Image img;
        if (get_image(png, path, &img)) return 1;
        int err = png_set_image(png, img);
        if (err) fprintf(stderr, "Error: encode '%s': %s\n", path, png_strerror(*png));
        image_free(img);
        return err;
}

PRIVATE int
embed(PNG *png, const char *path, Opts o)
{
        u8 *data;
        size_t len;
        if (read_file(o.embed, &data, &len)) return 1;

        int err;
        if (o.section) {
                err = png_add_section(png, o.section, data, len);
        } else {
                if (o.verbose) {
                        fprintf(stderr, "embedding %zu of %zu bytes available\n",
                                len, png_embed_capacity(png));
                }
                err = png_embed(png, data, len);
        }
        if (err) {
                fprintf(stderr, "Error: %s '%s': %s\n", o.section ? "add section to" : "embed in",
                        path, png_strerror(*png));
        } else if (o.verbose) {
                fprintf(stderr, "stored %zu bytes\n", len);
        }
        free(data);
        return err;
}

PRIVATE int
modify(const char *path, Opts o)
{
        PNG png;
        if (load(path, &png)) return 1;

        int err = 0;
        if (o.embed)
                err = embed(&png, path, o);
        else if (o.set_pixel)
                err = set_pixel(&png, path, o.set_pixel);
        else if (o.reencode)
                err = reencode(&png, path);

        if (!err) err = save(png, o.output);
        if (!err && o.verbose) fprintf(stderr, "wrote '%s'\n", o.output);
        png_free(png);
        return err;
}

PRIVATE int
extract(const char *path, Opts o)
{
        PNG png;
        if (load(path, &png)) return 1;

        u8 *data;
        size_t len;
        int err = o.section ? png_get_section(&png, o.section, &data, &len) : png_extract(&png, &data, &len);
        if (err) {
                fprintf(stderr, "Error: extract from '%s': %s\n", path, png_strerror(png));
                png_free(png);
                return 1;
        }

        err = write_file(o.extract, data, len);
        if (!err && o.verbose) fprintf(stderr, "extracted %zu bytes to '%s'\n", len, o.extract);
        free(data);
        png_free(png);
        return err;
}

PRIVATE const char *
check(Opts o, int npngs)
{
        int writes = !!o.embed + !!o.set_pixel + !!o.reencode;
        int views  = !!o.chunks + !!o.capacity + !!o.pixel;

        if (writes + !!o.extract > 1) return "use only one of --embed, --extract, --set-pixel, --reencode";
        if ((writes || o.extract || o.output) && views) return "--chunks, --capacity and --pixel only work when reading";
        if ((writes || o.output || o.extract) && npngs != 1) return "modifying or extracting takes a single png";
        if (writes && !o.output) return "--embed, --set-pixel and --reencode need --output";
        if (o.output && o.extract) return "--extract writes to its own file, not --output";
        if (o.section && !o.embed && !o.extract) return "--section needs --embed or --extract";
        return NULL;
}

int
main(int argc, char **argv)
{
        Opts o;

        flag_program(.help        = "Inspect a png, edit its pixels, or store data in it and get it back.\n"
                                    "With no options, print the header of each png. Data is stored in the\n"
                                    "2 low bits of every sample, or in a chunk if --section is given.\n"
                                    "--output alone rewrites the png.",
                     .positionals = flag_list("pngs"));
        flag_add(&o.verbose, "--verbose", "-v", .help = "print what is going on");
        flag_add(&o.chunks, "--chunks", "-c", .help = "list the chunks");
        flag_add(&o.capacity, "--capacity", NULL, .help = "print how many bytes --embed can store");
        flag_add(&o.pixel, "--pixel", "-p", .nargs = 1, .help = "print the samples of pixel X,Y");
        flag_add(&o.set_pixel, "--set-pixel", NULL, .nargs = 1, .help = "set pixel X,Y=V[,V...]");
        flag_add(&o.reencode, "--reencode", "-r", .help = "decode and compress the pixels again");
        flag_add(&o.embed, "--embed", "-e", .nargs = 1, .help = "store the contents of this file");
        flag_add(&o.extract, "--extract", "-x", .nargs = 1, .help = "write the stored data to this file");
        flag_add(&o.section, "--section", "-s", .nargs = 1, .help = "use a chunk with this 4 letter name");
        flag_add(&o.output, "--output", "-o", .nargs = 1, .help = "png to write");

        if (flag_parse(&argc, &argv)) {
                flag_show_help(STDERR_FILENO);
                return 1;
        }

        const char *usage = check(o, argc - 1);
        if (usage) {
                fprintf(stderr, "Error: %s\n", usage);
                flag_free();
                return 1;
        }

        int status = 0;
        if (o.output) {
                status = modify(argv[1], o);
        } else if (o.extract) {
                status = extract(argv[1], o);
        } else {
                for (int i = 1; i < argc; i++) {
                        if (argc > 2) printf("%s:\n", argv[i]);
                        if (show(argv[i], o)) status = 1;
                }
        }

        flag_free();
        return status;
}
