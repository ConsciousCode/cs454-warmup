CC = g++
CFLAGS = -Wall -Wextra -Werror -fno-exceptions -fno-rtti
WHICH = try.cpp

switch: $(WHICH)
	$(CC) $(CFLAGS) -o $@ $^

computed: $(WHICH)
	$(CC) $(CFLAGS) -o $@ $^ -DUSE_COMPUTED

vm.tar: hw1.c hw1.cpp um.py Makefile README.md test/
	tar -cvf $@ $^

clean: 
	rm -f switch computed

all: switch computed

.PHONY: clean all
