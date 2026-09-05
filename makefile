CC   = gcc
IDLC = idlc

IDL_SRC = messages.idl
IDL_GEN = messages.c messages.h

.PHONY: all clean

all: server client

$(IDL_GEN): $(IDL_SRC)
	$(IDLC) -l c $(IDL_SRC)

server: server.c messages.c
	$(CC) -o server server.c messages.c $(shell pkg-config --cflags --libs CycloneDDS)

client: client.c messages.c
	$(CC) client.c messages.c -o client $(shell pkg-config --cflags --libs glfw3) -lGL $(shell pkg-config --cflags --libs CycloneDDS) -pthread -lm

clean:
	rm -f server client messages.c messages.h
	rm -f certs/*.key certs/*.pem certs/*.p7s certs/*.csr certs/*.srl