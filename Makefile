CC = g++
CFLAGS = -Wall -Wextra -Werror -Ofast -fno-exceptions -fno-rtti -DUSE_COMPUTED
WHICH = try.cpp

um: $(WHICH)
	$(CC) $(CFLAGS) -o $@ $^

vm.tar: hw1.c hw1.cpp um.py Makefile README.md test/
	tar -cvf $@ $^

clean: 
	rm -f um

all: um

.PHONY: clean all
