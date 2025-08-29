I wrote my implementation in C.

## (Dis)assembler
I made a simple (dis)assembler which can be found with `vm.py`. It's run like:

```bash
python um.py asm [input.um] [output.asm]
python um.py dis [input.asm] (outputs to stdout)
```

This supports both the given canonical form and a simplified form with 3-letter mnemonics. For example, the first subroutine of `square.uma` would look like:

```
;; Common prelude
label @__start__
  ldi 0 0
  ldi 1 1
  ldi 5 4
  new 2 5
  ldi 7 @__end__
  sta 2 1 7
  ldi 4 @ummain
  prg 0 4
  label @__end__
  hlt
```

"label" is kept unshortened to allow it to stand out among the instructions. The 3-letter mnemonics are `mov`, `lda`, `sta`, `add`, `mul`, `div`, `nan`, `hlt`, `new`, `del`, `out`, `inp`, `prg`, and `ldi`. These can be intermixed with the canonical forms serving as aliases. All mnemonics are case-insensitive but labels are not.

### Disassembly
The disassembler outputs the 3-letter mnemonics. `ldi` always formats the immediate as a hex literal, but within `0x20` to `0x7E` (printable ASCII) shows a comment with the corresponding character literal as well. If a word is "non-canonical", meaning the unused 13 bits are non-zero, it's shown as a hex literal with a comment indicating its equivalent instruction to preserve round-trip ability. Invalid instructions are always printed as hex literals.

No attempt is made to try to map addresses to labels. The problem is labels are always loaded by the same `ldi` instruction which loads constants for calculation. Static analysis for determining if the value is used later by a `prg` instruction is unreliable with such a dynamic instruction set, but may be useful for future improvements.

### Assembly
`loadimm` supports binary (`0b`), octal (`0o`), hex (`0x`) literals, and character literals in single quotes (e.g. `'A'`). Escapes are not supported in character literals.

Literal words can be emitted by using hex literals prefixed with `0x`. This may be useful for embedding raw data in ordinarily executable code (eg for a lookup table) or allowing explicit use of the unused 13 bits in the middle of most instructions. Hex literals beyond the range of a word are truncated to the least significant 32 bits.

ASCII strings `"..."` are padded to the nearest word with null bytes and emitted as raw words. They can have backslash escapes with `\xhh` - no other escapes are supported.

## Making it faster
First I tried using `signal` to get rid of much of the bounds checking or combine multiple branches, but this had the unintended effect of pessimizing the compiler's optimization passes, resulting in noticeable performance hits.

I then went through and rearranged or removed superfluous branches, not much improvement there likely because they were easily predicted anyway.

Then I wanted to combine the whole VM's memory (index, program, arrays) into a single arena, but found the complexity of this overwhelming. I settled for separating the index (mapping indexes to offset + size) in its own array with the arrays occupying an arena and immediately compressing any holes to avoid the complexity of managing fragmentation. This ran into issues of correctness which I couldn't resolve in time, even using C++ to try to put some guardrails on.

I included `hw1.cpp` as proof of what I tried to do, but while it compiles it won't successfully run `sandmark.um` to completion.

I'm leaving the rest of the readme mostly as it was before. The performance is basically identical even with what I did end up changing.

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
### Part 1
After writing the bulk of the project, I got stuck with `sandmark.um` not running in full. I asked ChatGPT to analyze the code for oversights and it found these:
1. The `ldi` instruction uses 25 bits of immediate, not 24 (bits 0..24)
2. I missed one instance from an earlier implementation of the freelist using `size = 0` instead of `data = NULL` to mark an array as free.
3. I forgot to set `vm.free` in the growth branch, locking it into growing on every array allocation.
4. Failed to bounds-check the PC when using the switch implementation.

Once these were fixed everything worked correctly.

https://chatgpt.com/share/68a6ade6-8d08-8010-a246-b35217d28406

### Part 2
I tried to ask both ChatGPT and Claude to look for the underlying correctness issues but they were both catastrophically wrong about most of it for different reasons. I think they got confused by the memory layout meant for speed rather than clarity. In my experience, LLMs get easily tripped up when you do "weird" things even when they're correct, like trouble thinking outside the box.