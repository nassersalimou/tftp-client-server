CC := gcc
CFLAGS := -Wall -Wextra -Wpedantic -std=c11

.PHONY: all clean

all: client serveur

client: client.c
	$(CC) $(CFLAGS) -o $@ $<

serveur: serveur.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) client serveur
