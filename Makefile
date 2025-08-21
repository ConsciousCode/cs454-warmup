CC = gcc
CFLAGS = -Wall -Wextra -Werror # -Ofast

switch: hw1.c
	$(CC) $(CFLAGS) -o $@ $^

computed: hw1.c
	$(CC) $(CFLAGS) -o $@ $^ -DUSE_COMPUTED

vm.tar: hw1.c Makefile README.md test/
	tar -cvf $@ $^

clean: 
	rm -f switch computed

all: switch computed

.PHONY: clean all