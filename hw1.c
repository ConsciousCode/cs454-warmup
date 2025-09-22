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
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//#define USE_COMPUTED 1

// XXXX .... .... .... .... ...A AABB BCCC (generic)
// 1101 III. NNNN NNNN NNNN NNNN NNNN NNNN (ldi)
#define OPCODE(n) ((n) >> 28)
#define RX(n, x) (((x) >> ((n)*3)) & 0x7)
#define RI(x) (((x) >> 25) & 0x7)
#define RA(x) (RX(2, x))
#define RB(x) (RX(1, x))
#define RC(x) (RX(0, x))
#define REG(x) vm.registers[x]
#define REG_I() REG(RI(cur))
#define REG_A() REG(RA(cur))
#define REG_B() REG(RB(cur))
#define REG_C() REG(RC(cur))
#define IMM() (cur & 0x01ffffff) // Immediate value, 25 bits

#define OP_MOV   0 // A <- B unless C = 0
#define OP_LDA   1 // A <- B[C]
#define OP_STA   2 // A[B] <- C
#define OP_ADD   3 // A <- B + C mod 2^32
#define OP_MUL   4 // A <- B * C mod 2^32
#define OP_DIV   5 // A <- int(B / C)
#define OP_NAN  6 // A <- ~(B & C)
#define OP_HLT   7 // halt execution
#define OP_NEW   8 // B <- new reg_t[C]
#define OP_DEL   9 // delete C
#define OP_OUT  10 // putc(C)
#define OP_INP  11 // C <- getc(), -1 EOF
#define OP_PRG  12 // prog <- copy(B); PC <- C
#define OP_LDI  13 // A <- N

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

typedef struct {
    reg_t size;
    void *data;
} Array;

typedef struct {
    reg_t next;
    void *data;
} Freelist;

#define INDEX(type, arr, index) ((type *)((arr).data))[index]

void link_freelist(Freelist *free, reg_t i, reg_t last) {
    for(; i < last; ++i) {
        free[i].next = i + 1;
        free[i].data = NULL; // Inactive array
    }
    free[last].next = 0; // Last free
}

typedef struct {
    reg_t free;
    Array prog; // Cached program array
    Array arrays;

    reg_t pc;
    reg_t registers[8];
} VM;

const char *errname(Error err) {
    switch(err) {
        case ERR_OK: return "OK";
        case ERR_INV: return "Invalid instruction";
        case ERR_ARR: return "Inactive array identifier";
        case ERR_DEL: return "Deleted 0 or inactive array";
        case ERR_DIV: return "Division by zero";
        case ERR_PRG: return "Loaded program from inactive array";
        case ERR_CHR: return "Printed character outside of [0, 255]";
        case ERR_EOF: return "PC out of bounds";
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
        cur = INDEX(reg_t, vm.prog, vm.pc++); \
        SWITCH(cur); \
    } while(0)
    #define TARGET(op) TARGET_ ## op
#else
    #define DISPATCH_TABLE(...)
    #define DISPATCH_GOTO() continue
    #define TARGET(op) case op
    #define SWITCH(op) switch(OPCODE(op))
#endif

#define FAIL(err) do { error = err; goto finish; } while(0)

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
        reg_t cur = INDEX(reg_t, vm.prog, vm.pc++);
        //printop(cur);
        //printregs(&vm);
        SWITCH(cur) {
            TARGET(OP_MOV): {
                /**
                 * Tried (in ascending order of speed):
                 * - A = a ^ (a ^ B) & -!!C (9.767s)
                 * - A ^= (A ^ B) & -!!C (9.127s)
                 * - A = A & -!C | B & -!!C (9.019s)
                 * - if(C) A = B  (8.972s)
                 * - A = C? B : A (8.447s)
                **/
                REG_A() = REG_C()? REG_B() : REG_A();
                DISPATCH_GOTO();
            }

            TARGET(OP_LDA): {
                reg_t b = REG_B(), c = REG_C();
                if(b >= vm.arrays.size) FAIL(ERR_ARR);

                Array array = INDEX(Array, vm.arrays, b);
                if(/*array.data == NULL || */ c >= array.size) FAIL(ERR_ARR);

                REG_A() = INDEX(reg_t, array, c);
                DISPATCH_GOTO();
            }

            TARGET(OP_STA): {
                reg_t a = REG_A(), b = REG_B();
                if(a >= vm.arrays.size) FAIL(ERR_ARR);

                Array array = INDEX(Array, vm.arrays, a);
                if(/*array.data == NULL || */ b >= array.size) FAIL(ERR_ARR);

                INDEX(reg_t, array, b) = REG_C();
                DISPATCH_GOTO();
            }

            TARGET(OP_ADD):
                REG_A() = REG_B() + REG_C(); // Implicit mod
                DISPATCH_GOTO();

            TARGET(OP_MUL):
                REG_A() = REG_B() * REG_C();
                DISPATCH_GOTO();

            TARGET(OP_DIV): {
                reg_t c = REG_C();
                if(c == 0) FAIL(ERR_DIV);
                REG_A() = REG_B() / c;
                DISPATCH_GOTO();
            }

            TARGET(OP_NAN):
                REG_A() = ~(REG_B() & REG_C());
                DISPATCH_GOTO();

            TARGET(OP_HLT):
                goto finish;

            TARGET(OP_NEW): {
                reg_t index = vm.free;
                if(index) {
                    // Pop an array off the freelist
                    vm.free = INDEX(Freelist, vm.arrays, index).next;
                }
                else {
                    // Need to grow the arrays to fit a new one
                    reg_t size = vm.arrays.size;
                    index = size;
                    size *= 2;
                    vm.arrays = (Array){
                        .size = size,
                        .data = realloc(vm.arrays.data, size * sizeof(Array))
                    };
                    vm.free = index + 1;
                    link_freelist((Freelist *)vm.arrays.data, vm.free, size - 1);
                }

                // Allocate the new array
                reg_t size = REG_C();
                INDEX(Array, vm.arrays, index) = (Array){
                    .size = size,
                    .data = calloc(size, sizeof(reg_t))
                };
                REG_B() = index; // Store the new array index in B
                DISPATCH_GOTO();
            }

            TARGET(OP_DEL): {
                reg_t ident = REG_C();
                if(ident == 0) FAIL(ERR_DEL); // Attempted to delete the program

                Freelist *freearr = &INDEX(Freelist, vm.arrays, ident);
                free(freearr->data);
                *freearr = (Freelist){
                    .next = vm.free,
                    .data = NULL
                };
                vm.free = ident;
                DISPATCH_GOTO();
            }

            TARGET(OP_OUT): {
                reg_t c = REG_C();
                if(c > 0xff) FAIL(ERR_CHR); // Invalid character
                putchar(c);
                DISPATCH_GOTO();
            }

            TARGET(OP_INP): {
                int c = getchar();
                REG_C() = (c == EOF? -1 : c);
                DISPATCH_GOTO();
            }

            TARGET(OP_PRG): {
                // It's not explicitly stated but PRG 0 is a no-op aside from
                // assigning the PC, so it's likely intended to double as an
                // absolute jump.
                reg_t ident = REG_B();
                if(ident) {
                    if(ident >= vm.arrays.size) FAIL(ERR_ARR);

                    Array origin = INDEX(Array, vm.arrays, ident);
                    if(origin.data == NULL) FAIL(ERR_PRG);

                    // Free the current program
                    free(vm.prog.data);

                    // Copy the program data
                    reg_t size = origin.size;
                    void *data = malloc(size * sizeof(reg_t));
                    memcpy(data, origin.data, size * sizeof(reg_t));

                    // Assign it
                    vm.prog = INDEX(Array, vm.arrays, 0) = (Array){
                        .size = size,
                        .data = data
                    };
                }
                vm.pc = REG_C();
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

    reg_t *data = (reg_t *)malloc(size);

    for(size_t i = 0; i < size / sizeof(reg_t); ++i) {
        uint8_t buf[4];
        if(fread(buf, 1, 4, fp) < 4) {
            break;
        }

        data[i] = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    }

    fclose(fp);

    Array prog = {
        .size = (reg_t)(size / sizeof(reg_t)),
        .data = data
    };
    Array arrays = {
        .size = 256,
        .data = calloc(256, sizeof(Array))
    };
    INDEX(Array, arrays, 0) = prog;

    link_freelist((Freelist *)arrays.data, 1, arrays.size - 1);

    VM vm = {
        .free = 1,
        .prog = prog,
        .arrays = arrays,
        .pc = 0,
        .registers = {0}
    };
    Error err = interpret(vm);
    if(err) {
        fprintf(stderr, "%s\n", errname(err));
    }
    return err;
}
