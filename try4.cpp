/**
 * We make some simplifying assumptions for our interpreter. The specification
 * says that the arrays in memory are "each referenced by a distinct 32-bit
 * identifier", but because it never makes any mention of how to derive these
 * identifiers, we can treat these as offsets into a single contiguous array.
 * The only exception is the program code (array id 0) which the PRG instruction
 * can override "regardless of size". To avoid issues with trying to resize the
 * whole memory to fit an arbitrary program size, it's allocated into a separate
 * array.
 *
 * Allocation is handled by a free. I considered a heap allocator but thought
 * that would be overkill. Each allocated array is prefixed by its size, and
 * when in the free the first element is the offset of the next free block.
**/
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>

#include <bit>
#include <algorithm>

//#define USE_COMPUTED 1

// XXXX .... .... .... .... ...A AABB BCCC (generic)
// 1101 III. NNNN NNNN NNNN NNNN NNNN NNNN (ldi)
#define OPCODE(n) ((n) >> 28)
#define RX(n, x) (((x) >> ((n)*3)) & 7)
#define REG(x) vm.registers[x]
#define REG_I() REG((cur >> 25) & 7)
#define RA() REG(RX(2, cur))
#define RB() REG(RX(1, cur))
#define RC() REG(RX(0, cur))
#define IMM() (cur & 0x01ffffff) // Immediate value, 25 bits

#define OP_MOV  0 // A <- B unless C = 0
#define OP_LDA  1 // A <- B[C]
#define OP_STA  2 // A[B] <- C
#define OP_ADD  3 // A <- B + C mod 2^32
#define OP_MUL  4 // A <- B * C mod 2^32
#define OP_DIV  5 // A <- int(B / C)
#define OP_NAN  6 // A <- ~(B & C)
#define OP_HLT  7 // halt execution
#define OP_NEW  8 // B <- new reg_t[C]
#define OP_DEL  9 // delete C
#define OP_OUT 10 // putc(C)
#define OP_INP 11 // C <- getc(), -1 EOF
#define OP_PRG 12 // prog <- copy(B); PC <- C
#define OP_LDI 13 // A <- N

#define OP_INVALID 14
#define OP_x14  14
#define OP_x15  15 // Defined for completion's sake

enum Error {
    ERR_OK = 0,
    ERR_INV, // Invalid instruction
    ERR_ARR, // Inactive array identifier
    ERR_DEL, // Deleted 0 or inactive array
    ERR_DIV, // Division by zero
    ERR_PRG, // Loaded program from inactive array
    ERR_CHR, // Printed character outside of [0, 255]
    ERR_EOF  // PC out of bounds
};

typedef uint32_t word_t;

/**
 * Threadless Cache Malloc
 *
 * Inspired loosely by tcmalloc but dramatically simplified and without any
 * kind of thread safety.
 *
 * We split allocations into small and large objects around 4 kB. Small
 * objects are split into size classes of powers-of-two, each with its own
 * free list. New arenas are allocated in pages.
 *
 * Large objects are allocated using ordinary malloc.
**/
//*

#define LGOB 0xffff // Marker for large object
#define PAGE 4096
#define PAGE_MASK (PAGE - 1)
#define FULL_BM 0xffff
#define OVERHEAD 7

/**
 * Small object allocations are organized as divisions in a page. This allows
 * the derivation of the page (and its metadata) of an address.
**/
struct Page {
    Page *next; // May be garbage if not in a freelist

    uint32_t szclass;
    uint16_t nslots; // How many slots are in this size class
    uint16_t used; // How many slots are currently used

    uint64_t fullmask; // Which bitmaps are full (contain no free slots)
    uint64_t bitmap[8];

    // We set 2 words as the minimum allocation size, so treating this as
    // uint64_t instead of pairs of uint32_t makes the math easier.
    uint64_t data[];

    Page(uint32_t sz) {
        szclass = sz;
        if(sz != LGOB) {
            nslots = (PAGE - offsetof(Page, data))/sizeof(word_t)/(2 << sz);
        }
        else {
            nslots = 0; // Not used for large objects
        }
        used = 0;
        fullmask = 0;
        std::memset(bitmap, 0, sizeof(bitmap));
        // Only meaningful within a freelist, may be garbage
        //next = nullptr;
    }

    void set_free(word_t slot) {
        word_t bmx = slot / 64;
        bitmap[bmx] |= (1 << (slot % 64));
        // If something was freed, that block can't be full
        fullmask &= ~(1 << bmx);
        --used;
    }

    void set_used(word_t slot) {
        word_t bmx = slot / 64;
        bitmap[bmx] &= ~(1 << (slot % 64));
        if(bitmap[bmx] == UINT64_MAX) {
            fullmask |= (1 << bmx);
        }
        ++used;
    }

    word_t *pop_free() {
        word_t bmx = std::__countr_one(fullmask);
        assert(bmx < 8); // Must have at least one free slot
        word_t sub = std::__countr_one(bitmap[bmx]);
        word_t slot = (bmx << 6) | sub;
        assert(slot < nslots);
        set_used(slot);
        return (word_t *)&data[slot << szclass];
    }

    bool is_empty() {
        return used == 0;
    }

    bool is_full() {
        return used >= nslots;
    }
};

// Size classes of small objects - these form a linked list of pages. Every
// entry in this list has at least one free slot.
static Page *free_smob[13] = {0};

Page *tlc_page(void *ptr) {
    return (Page *)((uintptr_t)ptr & ~PAGE_MASK);
}

word_t *tlcmalloc(word_t words) {
    if(words <= PAGE/sizeof(word_t)/2) {
        // Saturate sizes 0-2 to szclass 0, otherwise do powers of two
        word_t szclass = 32 - std::__countl_zero(
            std::max(words - 1, 0u) >> 1
        );
        Page *page = free_smob[szclass];
        if(page == nullptr) {
            // Out of pages, make a new one
            page = (Page *)std::aligned_alloc(PAGE, PAGE);
            assert(page != nullptr);
            new (page) Page(szclass);
        }

        word_t *obj = page->pop_free();
        if(page->is_full()) {
            // Pop the page if it's now full
            free_smob[szclass] = page->next;
        }
        return obj;
    }
    else {
        // Allocate large objects using malloc
        word_t paged = (words + PAGE - 1) / PAGE * PAGE;
        Page *page = (Page *)std::aligned_alloc(PAGE, paged);
        assert(page != nullptr);
        new (page) Page(LGOB);
        return (word_t *)page->data;
    }
}

void tlcfree(word_t *ptr) {
    if(ptr == nullptr) return;

    Page *page = tlc_page(ptr);

    // Large object
    if(page->szclass == LGOB) {
        std::free(page);
        return;
    }

    // Small object
    word_t szclass = page->szclass;
    word_t index = ((uintptr_t)ptr & PAGE_MASK) >> szclass;

    bool was_full = page->is_full();
    page->set_free(index);

    // If the page was full, add it back to the freelist
    // If the page is empty and there are other pages, free this page
    // Otherwise it's already in the freelist

    if(was_full) {
        page->next = free_smob[szclass];
        free_smob[szclass] = page;
    }
    else if(page->is_empty()) {
        // Search for the page in the freelist
        word_t npages = 0;
        Page **indirect = &free_smob[szclass];
        while(*indirect && *indirect != page) {
            ++npages;
            indirect = &(*indirect)->next;
        }
        if(npages > 0) {
            *indirect = page->next;
            std::free(page);
        }
        else {
            // Only page, leave it in the freelist because we'll need it
            // later anyway
        }
    }
    else {
        // Already in the freelist and nothing to do
    }
}
//*/

struct Array {
    word_t size;
    word_t *data;

    Array(word_t initial_size = 0, word_t *initial_data = nullptr)
        : size(initial_size), data(initial_data) {
        if(initial_size && initial_data == nullptr) {
            data = (word_t *)tlcmalloc(initial_size);
        }
    }

    word_t &operator[](word_t index) {
        return data[index];
    }

    void copy(const Array &other) {
        Page *page = tlc_page(data);
        word_t szclass = page->szclass;
        if(szclass == LGOB || (2 << szclass) < other.size) {
            // Need to grow the memory to fit
            free();
            data = (word_t *)tlcmalloc(other.size);
        }
        size = other.size;
        std::memcpy(data, other.data, size);
    }

    void free() {
        tlcfree(data);
        size = 0;
        data = nullptr;
    }
};

/**
 * The array index grows and while realloc is possible with tlcmalloc, it's
 * more geared toward realloc for *smaller* sizes (eg the program array).
 * It makes more sense to use ordinary malloc/realloc instead.
**/
struct ArrayIndex {
    word_t size; // Size of the index
    Array *data; // Pointer to the data

    ArrayIndex() {
        size = 256;
        data = (Array *)std::calloc(256, sizeof(Array));
    }

    Array &operator[](word_t index) {
        return data[index];
    }

    void resize(word_t new_size) {
        size = new_size;
        data = (Array *)std::realloc(data, size * sizeof(Array));
    }
};

struct VM {
    word_t free;
    Array prog; // Cached program array
    ArrayIndex arrays;

    word_t pc;
    word_t registers[8];

    void set_next(word_t ident, word_t dst) {
        arrays[ident].size = dst - ident - 1;
    }

    word_t get_next(word_t ident) {
        return arrays[ident].size + ident + 1;
    }

    void push_free(word_t ident) {
        set_next(ident, free);
        free = ident;
    }

    word_t pop_new() {
        word_t ident = free;
        if(ident) {
            free = get_next(ident);
        }
        else {
            ident = arrays.size;
            free = ident + 1;
            arrays.resize(arrays.size * 2);
            set_next(arrays.size - 1, 0);
        }
        return ident;
    }
};

const char *errname(Error err) {
    switch(err) {
        case ERR_OK: return "OK";
        case ERR_INV: return "INV";
        case ERR_ARR: return "ARR";
        case ERR_DEL: return "DEL";
        case ERR_DIV: return "DIV";
        case ERR_PRG: return "PRG";
        case ERR_CHR: return "CHR";
        case ERR_EOF: return "EOF";
        default: return "Unknown error";
    }
}

/**
 * Saw this trick in the CPython interpreter years ago, seems to be more
 *  complicated now but a simpler version can be found at
 *  https://eli.thegreenplace.net/2012/07/12/computed-goto-for-efficient-dispatch-tables
 *
 * Speed-up is primarily because branch prediction is per-address, meaning if
 *  it's based on a single branch (eg the switch statement) the branch predictor
 *  can't predict anything effectively.
 *
 * However, switch is still faster on unoptimized builds. Computed goto is a
 *  couple seconds faster with -Ofast running sandmark.
**/
#if defined(USE_COMPUTED) && (defined(__GNUC__) || defined(__clang__))
    #define DISPATCH_TABLE(...) void *dispatch_table[] = {__VA_ARGS__}
    #define SWITCH(cur) goto *dispatch_table[OPCODE(cur)];
    #define DISPATCH_GOTO() do { \
        if(vm.pc >= vm.prog.size) FAIL(ERR_EOF); \
        cur = vm.prog[vm.pc++]; \
        SWITCH(cur); \
    } while(0)
    #define TARGET(op) TARGET_ ## op
#else
    #define DISPATCH_TABLE(...)
    #define DISPATCH_GOTO() continue
    #define TARGET(op) case op
    #define SWITCH(op) switch(OPCODE(op))
#endif

#define FAIL(err) do [[unlikely]] { error = err; goto finish; } while(0)

/**
 * Pass VM by value to avoid indirection overhead. Return for debug.
**/
Error interpret(VM vm) {
    Error error = ERR_OK;
    DISPATCH_TABLE(
        &&TARGET(OP_MOV),
        &&TARGET(OP_LDA), &&TARGET(OP_STA),
        &&TARGET(OP_ADD), &&TARGET(OP_MUL), &&TARGET(OP_DIV),
        &&TARGET(OP_NAN), &&TARGET(OP_HLT),
        &&TARGET(OP_NEW), &&TARGET(OP_DEL),
        &&TARGET(OP_OUT), &&TARGET(OP_INP),
        &&TARGET(OP_PRG), &&TARGET(OP_LDI),
        &&TARGET(OP_x14), &&TARGET(OP_x15)
    );

    do {
        word_t cur = vm.prog[vm.pc++];
        //printop(cur);
        //printregs(&vm);
        SWITCH(cur) {
            TARGET(OP_MOV): {
                /**
                 * Tried (Caps = REG_*(), lower = cached):
                 * - A = a ^ ((a ^ B) & -!!C) (9.767s)
                 * - A = REG(1 + !C) (9.444s)
                 * - A = REG(C? 1 : 2) (9.444s)
                 * - A = c? b : a (9.198s)
                 * - A ^= (A ^ B) & -!!C (9.127s)
                 * - A = A & -!C | B & -!!C (9.019s)
                 * - if(C) A = B (9.011s)
                 * - A = C? B : A (8.881s)
                 *
                 * I think this is the fastest because 1. The bitwise operations
                 * have overhead and 2. Ternary means it can assume RA() isn't
                 * UB so it can optimize better as an unconditional assignment.
                 * 
                 * Not actually sure why REG(C? 1 : 2) is slower though.
                 * Functionally that looks like R[A] = R[R[C]? B : A] and the
                 * assembly shows it uses cmove and no branches...
                **/
                RA() = RC()? RB() : RA();
                DISPATCH_GOTO();
            }
            
            TARGET(OP_LDA): {
                word_t b = RB(), c = RC();
                if(b >= vm.arrays.size) FAIL(ERR_ARR);

                auto array = vm.arrays[b];
                if(array.data == nullptr || c >= array.size) FAIL(ERR_ARR);
                
                RA() = array[c];
                DISPATCH_GOTO();
            }
            
            TARGET(OP_STA): {
                word_t a = RA(), b = RB();
                if(a >= vm.arrays.size) FAIL(ERR_ARR);

                auto array = vm.arrays[a];
                if(array.data == nullptr || b >= array.size) FAIL(ERR_ARR);

                printf("%p[%u] array[%u]\n", array.data, array.size, b);
                array[b] = RC();
                DISPATCH_GOTO();
            }
            
            TARGET(OP_ADD):
                RA() = RB() + RC(); // Implicit mod
                DISPATCH_GOTO();
            
            TARGET(OP_MUL):
                RA() = RB() * RC();
                DISPATCH_GOTO();
            
            TARGET(OP_DIV): {
                word_t c = RC();
                if(c == 0) FAIL(ERR_DIV);
                RA() = RB() / c;
                DISPATCH_GOTO();
            }
            
            TARGET(OP_NAN):
                RA() = ~(RB() & RC());
                DISPATCH_GOTO();
            
            TARGET(OP_HLT):
                goto finish;
            
            TARGET(OP_NEW): {
                word_t ident = vm.pop_new();
                vm.arrays[ident] = Array(RC());
                RB() = ident;
                DISPATCH_GOTO();
            }

            TARGET(OP_DEL): {
                word_t ident = RC();
                if(ident == 0) FAIL(ERR_DEL); // Attempted to delete the program
                
                vm.arrays[ident].free();
                vm.push_free(ident);
                DISPATCH_GOTO();
            }

            TARGET(OP_OUT): {
                word_t c = RC();
                if(c > 0xff) FAIL(ERR_CHR); // Invalid character
                putchar(c);
                DISPATCH_GOTO();
            }

            TARGET(OP_INP): {
                int c = getchar();
                RC() = (c == EOF? -1 : c);
                DISPATCH_GOTO();
            }

            TARGET(OP_PRG): {
                // It's not explicitly stated but PRG 0 is a no-op aside from
                // assigning the PC, so it's likely intended to double as an
                // absolute jump.
                if(word_t ident = RB()) {
                    if(ident >= vm.arrays.size) FAIL(ERR_ARR);

                    auto origin = vm.arrays[ident];
                    if(origin.data == nullptr) FAIL(ERR_PRG);

                    vm.prog.copy(origin);
                    vm.arrays[0] = vm.prog;
                }
                vm.pc = RC();
                DISPATCH_GOTO();
            }

            TARGET(OP_LDI):
                REG_I() = IMM();
                DISPATCH_GOTO();
            
            TARGET(OP_x14):
            TARGET(OP_x15):
                FAIL(ERR_INV);
        }
        
        // Fall-through if using switch
        FAIL(ERR_INV);
    } while(vm.pc < vm.prog.size);

    // Just in case
    FAIL(ERR_EOF);

    finish:
        return error;
}

int main(int argc, char *argv[]) {
    if(argc < 2) {
        fprintf(stderr, "Usage: %s <program>\n", argv[0]);
        return 0;
    }

    FILE *fp = fopen(argv[1], "rb");
    if(!fp) {
        perror("Failed to open program file");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    Array prog(size);

    for(size_t i = 0; i < size / sizeof(word_t); ++i) {
        uint8_t buf[4];
        if(fread(buf, 1, 4, fp) < 4) {
            break;
        }
        prog[i] = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    }

    fclose(fp);

    ArrayIndex arrays;
    arrays[0] = prog;

    VM vm = {
        .free = 1,
        .prog = prog,
        .arrays = arrays,
        .pc = 0,
        .registers = {0}
    };
    vm.set_next(255, 0);
    Error err = interpret(vm);
    if(err) {
        fprintf(stderr, "ERR_%s\n", errname(err));
    }
    return err;
}
