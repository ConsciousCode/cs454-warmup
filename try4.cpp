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

// XXXX .... .... .... .... ...A AABB BCCC (generic)
// 1101 III. NNNN NNNN NNNN NNNN NNNN NNNN (ldi)
#define OPCODE(n) ((n) >> 28)
#define RX(n, x) (((x) >> ((n)*3)) & 7)
#define REG(x) registers[x]
#define REG_I() REG((cur >> 25) & 7)
#define RA() REG(RX(2, cur))
#define RB() REG(RX(1, cur))
#define RC() REG(RX(0, cur))
#define IMM() (cur & 0x01ff'ffff) // Immediate value, 25 bits

#define OP_MOV  0 // A <- B unless C = 0
#define OP_LDA  1 // A <- B[C]
#define OP_STA  2 // A[B] <- C
#define OP_ADD  3 // A <- B + C
#define OP_MUL  4 // A <- B * C
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
        if(pc >= prog.size) FAIL(ERR_EOF); \
        cur = prog[pc++]; \
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

#define SZ(x) (2u << (x)) // Convert size class to max size in bytes

/**
 * Small object allocations are organized as divisions in a page. This allows
 * the derivation of the page (and its metadata) from an address within it.
**/
struct Page {
    // Large objects can never be in a freelist so we can store the size here{
    Page *next; // May be garbage if not in a freelist

    uint16_t nslots; // How many slots are in this size class
    uint16_t used; // How many slots are currently used

    uint16_t szclass;
    uint16_t fullmask; // Which bitmaps are full (contain no free slots)
    uint64_t bitmap[8];

    uint32_t data[];

    Page(uint32_t sz) {
        next = nullptr;
        szclass = sz;
        nslots = SZ(sz)? (PAGE - offsetof(Page, data))/SZ(sz) : 0;
        used = 0;
        fullmask = 0;
        std::memset(bitmap, 0, sizeof(bitmap));
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
        fullmask |= ((bitmap[bmx] == UINT64_MAX) << bmx);
        ++used;
    }

    word_t *pop_free() {
        word_t bmx = std::__countr_one(fullmask);
        assert(bmx < 8); // Must have at least one free slot
        word_t sub = std::__countr_one(bitmap[bmx]);
        assert(sub < 64); // Must have at least one free slot
        word_t slot = (bmx * 64) | sub;
        assert(slot < nslots);
        set_used(slot);
        return &data[slot * SZ(szclass) / sizeof(word_t)];
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
    return (Page *)((uintptr_t)ptr & ~(PAGE - 1));
}

word_t *tlcmalloc(word_t words) {
    if(words <= 256/sizeof(word_t)) {
        printf("tlcmalloc smob\n");
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
            printf("New page %d %p\n", szclass, page);
        }

        word_t *obj = page->pop_free();
        if(page->is_full()) {
            // Pop the page if it's now full
            free_smob[szclass] = page->next;
        }
        return obj;
    }
    else {
        printf("tlcmalloc lgob\n");
        // Allocate large objects using malloc
        word_t paged = (words + offsetof(Page, data) + PAGE - 1) / PAGE * PAGE;
        printf("lgob size %d -> %d\n", words, paged);
        Page *page = (Page *)std::aligned_alloc(PAGE, paged);
        assert(page != nullptr);
        new (page) Page(LGOB);
        page->lgob_size = words;
        return (word_t *)&page->data[0];
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
    word_t index = (ptr - page->data) / SZ(szclass);

    // If the page was full, add it back to the freelist
    // If the page is empty and there are other pages, free this page
    // Otherwise it's already in the freelist

    if(page->is_full()) {
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
            // Found it, remove from the freelist
            *indirect = page->next;
            std::free(page);
            return; // Avoid set_free because page was freed
        }
        else {
            // Only page, leave it in the freelist because we'll need it
            // later anyway
        }
    }
    else {
        // Already in the freelist and nothing to do
    }
    page->set_free(index);
}
//*/

struct ArrayPtr {
    word_t size;
    word_t *data;

    ArrayPtr(word_t initial_size = 0, word_t *initial_data = nullptr)
        : size(initial_size), data(initial_data) {
        if(initial_size && initial_data == nullptr) {
            data = tlcmalloc(initial_size);
            printf("memset (%p) %d\n", data, initial_size);
            std::memset(data, 0, initial_size * sizeof(word_t));
        }
    }

    word_t &operator[](word_t index) {
        return data[index];
    }

    void copy(const ArrayPtr &other) {
        Page *page = tlc_page(data);
        word_t szclass = page->szclass;
        if(szclass == LGOB || SZ(szclass)/sizeof(word_t) < other.size) {
            // Need to grow the memory to fit
            tlcfree(data);
            data = tlcmalloc(other.size);
        }
        size = other.size;
        std::memcpy(data, other.data, size*sizeof(word_t));
    }

    void free() {
        tlcfree(data);
        size = 0;
        data = nullptr;
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
 * The array index grows and while realloc is possible with tlcmalloc, it's
 * more geared toward realloc for *smaller* sizes (eg the program array).
 * It makes more sense to use ordinary malloc/realloc instead.
**/
struct ArrayIndex {
    word_t size; // Size of the index
    ArrayPtr *data; // Pointer to the data

    ArrayIndex() {
        size = 256;
        data = (ArrayPtr *)std::calloc(256, sizeof(ArrayPtr));
    }

    ArrayPtr &operator[](word_t index) {
        return data[index];
    }

    void resize(word_t new_size) {
        data = (ArrayPtr *)std::realloc(data, new_size * sizeof(ArrayPtr));
        for(word_t i = size; i < new_size; i++) {
            data[i] = ArrayPtr(0, nullptr);
        }
        size = new_size;
    }
};

struct VM {
    word_t free;
    ArrayPtr prog; // Cached program array
    ArrayIndex arrays;

    word_t pc;
    word_t registers[8];

    ~VM() {
        // Free all the arrays
        for(word_t i = 0; i < arrays.size; i++) {
            auto &d = arrays[i];
            if(d.data) d.free();
        }

        // Free any of the remaining pages
        for(word_t i = 0; i < 13; i++) {
            Page *it = free_smob[i];
            while(it) {
                Page *tmp = it->next;
                it = it->next;
                std::free(tmp);
            }
        }
    }

    /**
     * For freed arrays (data = nullptr), we maintain a linked list in
     * the array index using an index-relative pointer in the size field.
     * This allows us to treat zeroed out memory as an implicit linked list
     * to each adjacent index.
    **/
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

    Error interpret() {
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
            word_t cur = prog[pc++];
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
                    if(b >= arrays.size) FAIL(ERR_ARR);

                    auto array = arrays[b];
                    if(array.data == nullptr || c >= array.size) FAIL(ERR_ARR);

                    RA() = array[c];
                    DISPATCH_GOTO();
                }

                TARGET(OP_STA): {
                    word_t a = RA(), b = RB();
                    if(a >= arrays.size) FAIL(ERR_ARR);

                    auto array = arrays[a];
                    if(array.data == nullptr || b >= array.size) FAIL(ERR_ARR);

                    //auto *p = tlc_page(array.data);
                    //printf("%u = %u sta %d, %u (%x)\n", a, b, array.size, p->lgob_size, RC());
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
                    word_t ident = pop_new();
                    arrays[ident] = ArrayPtr(RC());
                    RB() = ident;
                    DISPATCH_GOTO();
                }

                TARGET(OP_DEL): {
                    word_t ident = RC();
                    if(ident == 0) FAIL(ERR_DEL); // Attempted to delete the program

                    arrays[ident].free();
                    push_free(ident);
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
                        //printf("prg %d\n", ident);
                        if(ident >= arrays.size) FAIL(ERR_ARR);

                        auto origin = arrays[ident];
                        if(origin.data == nullptr) FAIL(ERR_PRG);

                        prog.copy(origin);
                        arrays[0] = prog;
                    }
                    pc = RC();
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
        } while(pc < prog.size);

        // Just in case
        FAIL(ERR_EOF);

        finish:
            return error;
    }
};

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

    ArrayPtr prog(size/sizeof(word_t));

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
    Error err = vm.interpret();
    if(err) {
        fprintf(stderr, "ERR_%s\n", errname(err));
    }
    return err;
}
