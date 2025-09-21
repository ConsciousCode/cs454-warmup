CC = g++
CFLAGS = -Wall -Wextra -Werror -fno-exceptions -fno-rtti -DUSE_COMPUTED -g
WHICH = try4.cpp

um: $(WHICH)
	$(CC) $(CFLAGS) -o $@ $^

try4: try4.cpp
	$(CC) $(CFLAGS) -o $@ $^

vm.tar: hw1.c hw1.cpp um.py Makefile README.md test/
	tar -cvf $@ $^

clean:
	rm -f um

all: um

.PHONY: clean all
