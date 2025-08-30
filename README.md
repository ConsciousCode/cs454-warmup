I wrote my implementation in C.

## (Dis)assembler
I made a simple (dis)assembler which can be found with `vm.py`. It's run like:

```bash
$ python um.py asm [input.um] [output.asm]
$ python um.py dis [input.asm] (outputs to stdout)
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

I won't show the full disassembly of `sandmark.um`, but it has good examples of "non-canonical" instructions:

```
mov 3 2 0
add 3 0 0
ldi 1 0x14
ldi 2 0x5b ; '['
ldi 3 0x35 ; '5'
ldi 0 0x0d
prg 6 0
mov 0 0 0
0x5f0000d0 ; div 3 2 0
add 3 0 0
prg 6 1
prg 6 1
prg 6 1
prg 6 2
prg 6 3
prg 6 3
prg 6 3
mov 0 0 0
mov 0 0 0
mov 0 0 0
ldi 7 0x50 ; 'P'
ldi 0 0x4f ; 'O'
ldi 2 0x4c ; 'L'
ldi 3 0x41 ; 'A'
ldi 1 0x47 ; 'G'
ldi 5 0x44 ; 'D'
ldi 4 0x52 ; 'R'
```

That `0x5f0000d0` has a non-zero second nibble which is ignored by the VM but preserved by the disassembler for round-tripping. We can also see at the very end of the program:

```
0x9262e6ef ; del 7
0xe069b9bf
0x171ce019 ; lda 0 3 1
0xb85390ce ; inp 6
0x7e707687 ; hlt
0x05b70f17 ; mov 4 2 7
0xb9e00524 ; inp 4
0x01980017 ; mov 0 2 7
0xb5dc5157 ; inp 7
0x063edd62 ; mov 5 4 2
0xa05760e7 ; out 7
0x000778ae ; mov 2 5 6
0xa1a16440 ; out 0
0x00007fb4 ; mov 6 6 4
0x0160088d ; mov 2 1 5
0x01f40086 ; mov 2 0 6
0x1d6001fa ; lda 7 7 2
0x003280dd ; mov 3 3 5
0x39e620f2 ; add 3 6 2
0x4153d00c ; mul 0 1 4
0x9f00cf9d ; del 5
0xbf37ae5c ; inp 4
0xac3d8187 ; out 7
0x6a7d5503 ; nan 4 0 3
0x6f97de3b ; nan 0 7 3
0x33924da6 ; add 6 4 6
0x0013a8de ; mov 3 3 6
0x42a20c14 ; mul 0 2 4
0x01703497 ; mov 2 2 7
0x433c07b2 ; mul 6 6 2
0xc102f23d ; prg 7 5
ldi 6 0xc1bc93
0xb730d115 ; inp 5
0xa0c7dd31 ; out 1
0x43732041 ; mul 1 0 1
0xbce08c2c ; inp 4
0xb9974e12 ; inp 2
0xba76090e ; inp 6
0xc159824b ; prg 1 3
0xb04a9171 ; inp 1
0x8ba81e59 ; new 3 1
0xbf04c88c ; inp 4
0xbab8dc5d ; inp 5
0xc16e3846 ; prg 0 6
0x020ba951 ; mov 5 2 1
0x9f600679 ; del 1
0x5733b29a ; div 2 3 2
0x00005566 ; mov 5 4 6
0xffffffff
0x99999999 ; del 1
```

What very clearly looks to be some kind of lookup table.

### Assembly
`loadimm` supports binary (`0b`), octal (`0o`), hex (`0x`) literals, and character literals in single quotes (e.g. `'A'`). Escapes are not supported in character literals.

Literal words can be emitted by using hex literals prefixed with `0x`. This may be useful for embedding raw data in ordinarily executable code (eg for a lookup table) or allowing explicit use of the unused 13 bits in the middle of most instructions. Hex literals beyond the range of a word are truncated to the least significant 32 bits.

ASCII strings `"..."` are padded to the nearest word with null bytes and emitted as raw words. They can have backslash escapes with `\xhh` - no other escapes are supported.

I thought I would take the initiative and write a fizzbuzz program:

```
ldi 0 0
ldi 1 1
ldi 2 1 ; i

label @main:loop
label @check_15
    ldi 3 15
    div 4 2 3
    mul 4 4 3 ;; r4 = r2 / 15 * 15

    nan 4 4 4
    add 4 4 1
    add 4 2 4 ;; r4 = r2 - r4

    ldi 5 @print_fizzbuzz
    ldi 6 @check_3
    mov 5 6 4
    prg 0 5 ;; goto (r4? r6 : r5)

label @print_fizzbuzz
    ldi 3 'F'
    out 3
    ldi 3 'i'
    out 3
    ldi 3 'z'
    out 3
    out 3
    ldi 4 'B'
    out 4
    ldi 4 'u'
    out 4
    out 3
    out 3

    ldi 5 @main:continue
    prg 0 5

label @check_3
    ldi 3 3
    div 4 2 3
    mul 4 4 3 ;; r4 = r2 / 3 * 3

    nan 4 4 4
    add 4 4 1
    add 4 2 4 ;; r4 = r2 - r4

    ldi 5 @print_fizz
    ldi 6 @check_5
    mov 5 6 4
    prg 0 5 ;; goto (r4? r6 : r5)

label @print_fizz
    ldi 3 'F'
    out 3
    ldi 3 'i'
    out 3
    ldi 3 'z'
    out 3
    out 3

    ldi 5 @main:continue
    prg 0 5

label @check_5
    ldi 3 5
    div 4 2 3
    mul 4 4 3 ;; r4 = r2 / 5 * 5

    nan 4 4 4
    add 4 4 1
    add 4 2 4 ;; r4 = r2 - r4

    ldi 5 @print_buzz
    ldi 6 @print_num
    mov 5 6 4
    prg 0 5 ;; goto (r4? r6 : r5)

label @print_buzz
    ldi 3 'B'
    out 3
    ldi 3 'u'
    out 3
    ldi 3 'z'
    out 3
    out 3

    ldi 5 @main:continue
    prg 0 5

label @print_num
    mov 3 2 1 ;; r3 = i
    ldi 7 0 ; Linked list of cons cells, (car . cdr)

;;; Unpack the integer into a reversed linked list of digits
label @print_num:digitize
    ldi 5 10
    div 4 3 5
    mul 4 4 5 ;; r4 = r2 / 10 * 10

    nan 4 4 4
    add 4 4 1
    add 4 3 4 ;; r4 = r3 - r4

    ldi 6 2
    new 6 6
    sta 6 0 4
    sta 6 1 7
    mov 7 6 1 ;; r7 = (r4 . r7)

    div 3 3 5 ;; r3 = r3 / 10

    ldi 5 @print_num:print
    ldi 6 @print_num:digitize
    mov 5 6 3
    prg 0 5

;;; Pop digits off the list to print them in-order
label @print_num:print
    lda 4 7 0
    ldi 3 '0'
    add 4 4 3
    out 4 ;; putc(car r7 + '0')

    lda 4 7 1
    del 7
    mov 7 4 1 ;; r7 = pop()
    
    ldi 5 @main:continue
    ldi 6 @print_num:print
    mov 5 6 4
    prg 0 5

label @main:continue
    ldi 4 0x0A
    out 4

    add 2 2 1 ;; i = i + 1

    ldi 3 101
    nan 3 3 3
    add 3 3 1
    add 3 2 3 ;; r3 = i - 101

    ldi 5 @main:break
    ldi 6 @main:loop
    mov 5 6 3
    prg 0 5 ;; if(i == 101) break

label @main:break
    hlt
```

And this can be assembled and tested by using the following commands:

```bash
$ python um.py asm test/fizzbuzz.uma test/fizzbuzz.um
$ ./computed test/fizzbuzz.um
```

Resulting in this disassembly:

```
$ python um.py dis test/fizzbuzz.um
ldi 0 0x00
ldi 1 0x01
ldi 2 0x01
ldi 3 0x0f
div 4 2 3
mul 4 4 3
nan 4 4 4
add 4 4 1
add 4 2 4
ldi 5 0x0d
ldi 6 0x1c
mov 5 6 4
prg 0 5
ldi 3 0x46 ; 'F'
out 3
ldi 3 0x69 ; 'i'
out 3
ldi 3 0x7a ; 'z'
out 3
out 3
ldi 4 0x42 ; 'B'
out 4
ldi 4 0x75 ; 'u'
out 4
out 3
out 3
ldi 5 0x5f ; '_'
prg 0 5
ldi 3 0x03
div 4 2 3
mul 4 4 3
nan 4 4 4
add 4 4 1
add 4 2 4
ldi 5 0x26 ; '&'
ldi 6 0x2f ; '/'
mov 5 6 4
prg 0 5
ldi 3 0x46 ; 'F'
out 3
ldi 3 0x69 ; 'i'
out 3
ldi 3 0x7a ; 'z'
out 3
out 3
ldi 5 0x5f ; '_'
prg 0 5
ldi 3 0x05
div 4 2 3
mul 4 4 3
nan 4 4 4
add 4 4 1
add 4 2 4
ldi 5 0x39 ; '9'
ldi 6 0x42 ; 'B'
mov 5 6 4
prg 0 5
ldi 3 0x42 ; 'B'
out 3
ldi 3 0x75 ; 'u'
out 3
ldi 3 0x7a ; 'z'
out 3
out 3
ldi 5 0x5f ; '_'
prg 0 5
mov 3 2 1
ldi 7 0x00
ldi 5 0x0a
div 4 3 5
mul 4 4 5
nan 4 4 4
add 4 4 1
add 4 3 4
ldi 6 0x02
new 6 6
sta 6 0 4
sta 6 1 7
mov 7 6 1
div 3 3 5
ldi 5 0x54 ; 'T'
ldi 6 0x44 ; 'D'
mov 5 6 3
prg 0 5
lda 4 7 0
ldi 3 0x30 ; '0'
add 4 4 3
out 4
lda 4 7 1
del 7
mov 7 4 1
ldi 5 0x5f ; '_'
ldi 6 0x54 ; 'T'
mov 5 6 4
prg 0 5
ldi 4 0x0a
out 4
add 2 2 1
ldi 3 0x65 ; 'e'
nan 3 3 3
add 3 3 1
add 3 2 3
ldi 5 0x6a ; 'j'
ldi 6 0x03
mov 5 6 3
prg 0 5
hlt
```

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
Running `sandmark.um` on b146-46.cs.unm.edu:
```bash
rmcdan04@b146-46:~/Desktop/cs454-warmup$ make computed CFLAGS="-Ofast"
gcc -Ofast -o computed hw1.c -DUSE_COMPUTED
rmcdan04@b146-46:~/Desktop/cs454-warmup$ date
Fri Aug 29 04:13:52 PM MDT 2025
rmcdan04@b146-46:~/Desktop/cs454-warmup$ time ./computed test/sandmark.um
trying to Allocate array of size 0..
trying to Abandon size 0 allocation..
trying to Allocate size 11..
trying Array Index on allocated array..
trying Amendment of allocated array..
checking Amendment of allocated array..
trying Alloc(a,a) and amending it..
comparing multiple allocations..
pointer arithmetic..
check old allocation..
simple tests ok!
about to load program from some allocated array..
success.
verifying that the array and its copy are the same...
success.
testing aliasing..
success.
free after loadprog..
success.
loadprog ok.
 == SANDmark 19106 beginning stress test / benchmark.. ==
100. 12345678.09abcdef
99.  6d58165c.2948d58d
98.  0f63b9ed.1d9c4076
97.  8dba0fc0.64af8685
96.  583e02ae.490775c0
95.  0353a77b.2f02685c
94.  aa25a8d7.51cb07e5
93.  e13149f5.53a9ae5d
92.  abbbd460.86cf279c
91.  2c25e8d8.a71883a9
90.  dccf7b71.475e0715
89.  49b398a7.f293a13d
88.  9116f443.2d29be37
87.  5c79ba31.71e7e592
86.  19537c73.0797380a
85.  f46a7339.fe37b85a
84.  99c71532.729e2864
83.  f3455289.b84ced3d
82.  c90c81a9.b66fcd61
81.  087e9eef.fc1c13a6
80.  e933e2f5.3567082f
79.  25af849e.16290d7b
78.  57af9504.c76e7ded
77.  68cf6c69.6055d00c
76.  8e920fbd.02369722
75.  eb06e2de.03c46fda
74.  f9c40240.f1290b2a
73.  7f484f97.bc15610b
72.  1dabb00e.61e7b75b
71.  dceb40f5.207a75ca
70.  c3ed44f5.db631e81
69.  b7addb67.90460bf5
68.  ae710a90.04b433ef
67.  9ca2d5f0.05d3b631
66.  4f38abe0.4287cc05
65.  10d8691d.a5c934f8
64.  27c68255.52881eaa
63.  a0695283.110266b7
62.  336aa5dd.57287a9b
61.  b04fe494.d741ddbd
60.  2baf3654.9e33305a
59.  fd82095d.683efb19
58.  d0bac37f.badff9d7
57.  3be33fcc.d76b127e
56.  7f964f18.8b118ee1
55.  37aeddc8.26a8f840
54.  d71d55ff.6994c78f
53.  bf175396.f960cc54
52.  f6c9d8e1.44b81fd5
51.  6a9b4d86.fe7c66cb
50.  06bceb64.d5106aad
49.  237183b6.49c15b01
48.  4ec10756.6936136f
47.  9d1855a7.1e929fe8
46.  a641ede3.36bff422
45.  7bbf5ad4.dd129538
44.  732b385e.39fadce7
43.  b7f50285.e7f54c39
42.  42e3754c.da741dc1
41.  5dc42265.928ea0bb
40.  623fb352.3f25bc5b
39.  491f33d9.409bca87
38.  f0943bc7.89f512be
37.  80cdbc9d.8ad93517
36.  c1a8da99.32d37f3f
35.  91a0b15c.6df2cf4e
34.  50cf7a7a.f0466dc8
33.  02df4c13.14eb615d
32.  2963bf25.d9f06dfe
31.  c493d2db.f39ce804
30.  3b6e5a8e.5cf63bd7
29.  4c5c2fbe.8d881c00
28.  9b7354a6.81181438
27.  ae0fe8c6.ec436274
26.  e786b98d.f5a4111d
25.  a7719df1.d989d0b6
24.  beb9ebc0.6c56750d
23.  edf41fcb.e4cba003
22.  97268c46.713025f1
21.  deb087db.1349eb6a
20.  fc5221f0.3b4241bf
19.  3fa4370d.8fa16752
18.  044af7de.87b44b11
17.  2e86e437.c4cdbc54
16.  fd7cd8aa.63b6ca23
15.  631ceaad.e093a9d5
14.  01ca9732.52962532
13.  86d8bcf5.45bdf474
12.  8d07855b.0224e80f
11.  0f9d2bee.94d86c38
10.  5e6a685d.26597494
9.   24825ea1.72008775
8.   73f9c0b5.1480e7a3
7.   a30735ec.a49b5dad
6.   a7b6666b.509e5338
5.   d0e8236e.8b0e9826
4.   4d20f3ac.a25d05a8
3.   7c7394b2.476c1ee5
2.   f3a52453.19cc755d
1.   2c80b43d.5646302f
0.   a8d1619e.5540e6cf
SANDmark complete.

real	0m9.932s
user	0m9.925s
sys	0m0.004s
```

2 whole seconds slower on b146-46 than on my laptop.

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