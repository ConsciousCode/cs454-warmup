I wrote my implementation in C.

## Implementation
Originally I put all the arrays in a single memory block, using the specification's ambivalence about identifiers to allow referencing them by *offset*. However because arrays must be checked for being "active", this created a lot of complexity and indirection overhead anyway. Instead, I went with a more straightforward array of arrays which means the 0 array (program) no longer needs special consideration. When these are freed, they're pushed to a free list (acting as a stack) with `Freelist::next` serving as the offset of the next free array slot.

I also tried a trick I learned reading the CPython interpreter a few years ago: using computed gotos to leverage branch prediction instead of funneling every loop through a single switch statement. Unoptimized, this actually performs 28% worse, but 2.5% faster with `-Ofast`.

## Performance
I wasn't able to ssh into callisto so I ran the tests on my laptop. Running `sandmark.um`:

```bash
$ make all
$ time ./switch test/sandmark.um
...
real	0m16.558s
user	0m16.554s
sys	0m0.003s
$ time ./computed test/sandmark.um
...
real	0m21.164s
user	0m21.141s
sys	0m0.017s
$ make clean
$ make all CFLAGS+="-Ofast"
$ time ./switch test/sandmark.um
...
real	0m10.527s
user	0m10.520s
sys	0m0.006s
$ time ./computed test/sandmark.um
...
real	0m8.819s
user	0m8.814s
sys	0m0.005s
```

## AI disclosure
After writing the bulk of the project, I got stuck with `sandmark.um` not running in full. I asked ChatGPT to analyze the code for oversights and it found these:
1. The `ldi` instruction uses 25 bits of immediate, not 24 (bits 0..24)
2. I missed one instance from an earlier implementation of the freelist using `size = 0` instead of `data = NULL` to mark an array as free.
3. I forgot to set `vm.free` in the growth branch, locking it into growing on every array allocation.
4. Failed to bounds-check the PC when using the switch implementation.

Once these were fixed everything worked correctly.

https://chatgpt.com/share/68a6ade6-8d08-8010-a246-b35217d28406