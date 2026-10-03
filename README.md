# libpngstore

Read, edit and write PNG files, and store arbitrary data inside them.

Data can go in two places:

- Pixels: the 2 low bits of every sample. The image looks the same.
  Works on 8 and 16 bit, non-palette images.
- Section: a private ancillary chunk before `IEND`, ignored by viewers.

## Build

Needs zlib.

```sh
make # ./pngstore, build/lib/libpngstore.{a,so}
```

## CLI

```sh
pngstore image.png                              # header info
pngstore -c --capacity -p 10,10 image.png       # chunks, capacity, a pixel

pngstore -e secret.bin -o out.png image.png     # hide in the pixels
pngstore -x secret.bin out.png                  # get it back

pngstore -e notes.txt -s note -o out.png image.png
pngstore -x notes.txt -s note out.png

pngstore --set-pixel 10,10=255,0,0,255 -o out.png image.png
pngstore -r -o out.png image.png                # re-encode the pixels
```

Run `pngstore -h` for every option.

## Library

```c
#include "libpngstore.h"

PNG png = png_read(fopen("image.png", "rb"));
png_add_section(&png, "note", data, len);
png_embed(&png, secret, secret_len);
png_write(png, fopen("out.png", "wb"));
png_free(png);
```

Functions return non-zero on error; `png_strerror(png)` describes it.
