CC = gcc
CFLAGS = -Wall -Wextra -Werror #-fno-exceptions -fno-rtti

switch: hw1.c
	$(CC) $(CFLAGS) -o $@ $^

computed: hw1.c
	$(CC) $(CFLAGS) -o $@ $^ -DUSE_COMPUTED

vm.tar: hw1.c hw1.cpp um.py Makefile README.md test/
	tar -cvf $@ $^

clean: 
	rm -f switch computed

all: switch computed vm.tar

.PHONY: clean all
