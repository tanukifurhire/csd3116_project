CC      = gcc
IDLC    = idlc

IDL_SRC = messages.idl
IDL_GEN = messages.c messages.h

CFLAGS_DDS  = $(shell pkg-config --cflags CycloneDDS)
LIBS_DDS    = $(shell pkg-config --libs CycloneDDS)
CFLAGS_GLFW = $(shell pkg-config --cflags glfw3)
LIBS_GLFW   = $(shell pkg-config --libs glfw3)

.PHONY: all clean

all: server client

# Regenerate messages.c/.h only if messages.idl changed
$(IDL_GEN): $(IDL_SRC)
	$(IDLC) -l c $(IDL_SRC)

server: server.c messages.c
	$(CC) -o server server.c messages.c $(CFLAGS_DDS) $(LIBS_DDS)

client: client.c messages.c
	$(CC) client.c messages.c -o client $(CFLAGS_GLFW) -lGL $(CFLAGS_DDS) $(LIBS_DDS) -pthread -lm

clean:
	rm -f server client messages.c messages.h