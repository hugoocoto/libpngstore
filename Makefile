CC      ?= gcc
AR      ?= ar
CFLAGS  += -Wall -Wextra -ggdb -fPIC
LDFLAGS ?=
LDLIBS  ?=
LDLIBS  += -lz

SRCDIR   = src
BUILDDIR = build
LIBDIR   = $(BUILDDIR)/lib
NAME     = pngstore

BIN    = $(NAME)
STATIC = $(LIBDIR)/lib$(NAME).a
SHARED = $(LIBDIR)/lib$(NAME).so

SRC     = $(wildcard $(SRCDIR)/*.c)
OBJ     = $(SRC:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
LIB_OBJ = $(filter-out $(BUILDDIR)/main.o, $(OBJ))
DEP     = $(OBJ:.o=.d)

.PHONY: all lib clean

all: $(BIN) lib

lib: $(STATIC) $(SHARED)

$(BIN): $(BUILDDIR)/main.o $(STATIC)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(STATIC): $(LIB_OBJ) | $(LIBDIR)
	$(AR) rcs $@ $^

$(SHARED): $(LIB_OBJ) | $(LIBDIR)
	$(CC) $(LDFLAGS) -shared -o $@ $^ $(LDLIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c Makefile | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR) $(LIBDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR) $(BIN)

-include $(DEP)
