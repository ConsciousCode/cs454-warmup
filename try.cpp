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
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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

typedef enum {
    ERR_OK = 0,
    ERR_INV, // Invalid instruction
    ERR_ARR, // Inactive array identifier
    ERR_DEL, // Deleted 0 or inactive array
    ERR_DIV, // Division by zero
    ERR_PRG, // Loaded program from inactive array
    ERR_CHR, // Printed character outside of [0, 255]
    ERR_EOF  // PC out of bounds
} Error;

typedef uint32_t reg_t;

template<typename T>
struct Array {
    reg_t size; // Size of the array
    T *data; // Pointer to the data

    Array(reg_t initial_size = 0, T *initial_data = nullptr)
        : size(initial_size), data(initial_data) {
        if(initial_size && initial_data == nullptr) {
            data = (T *)std::calloc(initial_size, sizeof(T));
        }
    }

    T &operator[](reg_t index) {
        return data[index];
    }

    void resize(reg_t new_size) {
        size = new_size;
        data = (T *)std::realloc(data, size * sizeof(T));
    }

    void copy(const Array<T> &other) {
        size = other.size;
        data = (T *)std::realloc(data, size * sizeof(T));
        std::memcpy(data, other.data, size * sizeof(T));
    }

    void move(reg_t dst, reg_t src, reg_t count) {
        std::memmove(&data[dst], &data[src], count * sizeof(T));
    }

    void clear(reg_t dst, reg_t count) {
        std::memset(&data[dst], T(), count * sizeof(T));
    }

    void free() {
        std::free(data);
        size = 0;
        data = nullptr;
    }
};

struct VM {
    reg_t free;
    Array<reg_t> prog; // Cached program array
    Array<Array<reg_t>> arrays;

    reg_t pc;
    reg_t registers[8];

    void set_next(reg_t ident, reg_t dst) {
        arrays[ident].size = dst - ident - 1;
    }

    reg_t get_next(reg_t ident) {
        return arrays[ident].size + ident + 1;
    }

    void push_free(reg_t ident) {
        set_next(ident, free);
        free = ident;
    }

    reg_t pop_new() {
        reg_t ident = free;
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
        reg_t cur = vm.prog[vm.pc++];
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
                reg_t b = RB(), c = RC();
                if(b >= vm.arrays.size) FAIL(ERR_ARR);

                auto array = vm.arrays[b];
                if(array.data == nullptr || c >= array.size) FAIL(ERR_ARR);
                
                RA() = array[c];
                DISPATCH_GOTO();
            }
            
            TARGET(OP_STA): {
                reg_t a = RA(), b = RB();
                if(a >= vm.arrays.size) FAIL(ERR_ARR);

                auto array = vm.arrays[a];
                if(array.data == nullptr || b >= array.size) FAIL(ERR_ARR);

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
                reg_t c = RC();
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
                reg_t ident = vm.pop_new();
                vm.arrays[ident] = Array<reg_t>(RC());
                RB() = ident;
                DISPATCH_GOTO();
            }

            TARGET(OP_DEL): {
                reg_t ident = RC();
                if(ident == 0) FAIL(ERR_DEL); // Attempted to delete the program
                
                vm.arrays[ident].free();
                vm.push_free(ident);
                DISPATCH_GOTO();
            }

            TARGET(OP_OUT): {
                reg_t c = RC();
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
                if(reg_t ident = RB()) {
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

    Array<reg_t> prog(size);

    for(size_t i = 0; i < size / sizeof(reg_t); ++i) {
        uint8_t buf[4];
        if(fread(buf, 1, 4, fp) < 4) {
            break;
        }
        prog[i] = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    }

    fclose(fp);

    Array<Array<reg_t>> arrays(256);
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
