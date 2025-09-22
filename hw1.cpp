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

typedef uint32_t reg_t;

template<typename T>
struct Array {
    reg_t size; // Size of the array
    T *data;   // Pointer to the data

    Array(reg_t initial_size = 0, T *initial_data = nullptr)
        : size(initial_size), data(initial_data) {
        if(data == nullptr) {
            data = (T *)calloc(size, sizeof(T));
        }
    }

    T &operator[](reg_t index) {
        return data[index];
    }

    void resize(reg_t new_size) {
        data = (T *)realloc(data, new_size * sizeof(T));
        for(reg_t i = size; i < new_size; ++i) {
            data[i] = T(); // Initialize new elements
        }
        size = new_size;
    }

    void move(reg_t dst, reg_t src, reg_t count) {
        memmove(&data[dst], &data[src], count * sizeof(T));
    }

    void clear(reg_t dst, reg_t count) {
        memset(&data[dst], T(), count * sizeof(T));
    }
};

typedef union {
    struct {
        reg_t size;
        reg_t offset;
    };

    struct {
        int32_t next; // Next free identifier
        reg_t is_active; // Zero for inactive arrays except for array 0 (program)
    };
} ArrayDef;

//*
struct VM {
    reg_t free;
    reg_t capacity; // Capacity of the program
    reg_t progsize; // Size of the program
    Array<ArrayDef> index; // Array definitions
    reg_t unused; // How much of the memory is unused
    Array<reg_t> memory; // Memory for arrays

    reg_t pc;
    reg_t registers[8];

    void set_next(reg_t ident, reg_t dst) {
        index[ident].next = dst - ident - 1;
    }

    reg_t get_next(reg_t ident) {
        return index[ident].next + ident + 1;
    }

    void push_free(reg_t ident) {
        set_next(ident, free);
        free = ident;
    }

    reg_t push_new() {
        reg_t ident = free;
        if(ident) {
            free = get_next(ident);
        }
        else {
            // Freelist is empty, need to grow
            ident = index.size;
            free = ident + 1;
            index.resize(index.size*2);
            set_next(index.size - 1, 0); // Last points to 0
        }
        return ident;
    }

    void shrink_hole(reg_t offset, reg_t size) {
        reg_t used = memory.size - unused;
        reg_t end = offset + size;
        // Shift the memory to avoid holes
        memory.move(offset, end, used - end);

        // Update the index (including the deleted array)
        // This won't touch inactive arrays because their offset = 0
        // (skip array 0)
        for(reg_t i = 1; i < index.size; ++i) {
            if(index[i].offset >= offset) {
                // if offset == start (deleted array), now offset = 0
                index[i].offset -= size;
            }
        }

        unused += size;
    }

    /**
     * Allocate enough memory to fit an object of the given size
    **/
    void alloc_memory(reg_t size) {
        if(unused < size) {
            // Fill the unused space, then grow to be twice what would be
            // needed to fit the rest of the new size
            reg_t used = memory.size - unused;
            reg_t msize = used + size;
            memory.resize(msize*2);
            unused = memory.size - used;
        }
        unused -= size;
    }

    /**
     * Grow the program array's capacity to receive a bigger program.
    **/
    void grow_program(reg_t new_capacity) {
        reg_t start = capacity, end = new_capacity;
        reg_t extra = end - start;
        alloc_memory(extra);
        // Move everything after the program to accomodate the new program.
        reg_t used = memory.size - unused;
        // Used memory after the program
        memory.move(end, start,used - start);
        // Update the offsets (skip array 0)
        for(reg_t i = 1; i < index.size; ++i) {
            if(index[i].is_active) {
                index[i].offset += extra;
            }
        }
        capacity = new_capacity;
    }

    void print_state() {
        printf("arrays { %d }\n", index.size);
        /*
        printf("arrays { ");
        for(reg_t i = 0; i < index.size; ++i) {
            if(index[i].is_active || i == 0) {
                printf("%u: (off=%u size=%u) ", i, index[i].offset, index[i].size);
            }
        }
        printf("}\n");
        */
        printf("PC=%u | free=%u | progsize=%u | capacity=%u | unused=%u\n", pc, free, progsize, capacity, unused);
        for(reg_t i = 0; i < 8; ++i) {
            printf("R%u=%u ", i, registers[i]);
        }
        printf("\n");
    }
};
//*/

/*
typedef struct {
    reg_t free;
    Array prog;
    Array arrays;

    reg_t pc;
    reg_t registers[8];
} VM;
//*/

const char *opname(reg_t code) {
    switch(OPCODE(code)) {
        case OP_MOV: return "MOV";
        case OP_LDA: return "LDA";
        case OP_STA: return "STA";
        case OP_ADD: return "ADD";
        case OP_MUL: return "MUL";
        case OP_DIV: return "DIV";
        case OP_NAN: return "NAN";
        case OP_HLT: return "HLT";
        case OP_NEW: return "NEW";
        case OP_DEL: return "DEL";
        case OP_OUT: return "OUT";
        case OP_INP: return "INP";
        case OP_PRG: return "PRG";
        case OP_LDI: return "LDI";
        case OP_x14: return "x14";
        case OP_x15: return "x15";
        default: return "???";
    }
}

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
    #define SWITCH(cur) goto *dispatch_table[OPCODE(cur)]; if(0) {} else
    #define DISPATCH_GOTO() do { \
        if(vm.pc >= vm.progsize) FAIL(ERR_EOF); \
        cur = vm.memory[vm.pc++]; \
        SWITCH(cur) {} \
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
    //printf("Prog size %u | free %u\n", vm.progsize, vm.free);
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
        reg_t cur = vm.memory[vm.pc++];
        //printf("%u pc=%u (%u)\n", OPCODE(cur), vm.pc, vm.progsize);
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
                 * have overhead and 2. Ternary means it can assume REG_A() isn't
                 * UB so it can optimize better as an unconditional assignment.
                 *
                 * Not actually sure why REG(C? 1 : 2) is slower though.
                 * Functionally that looks like R[A] = R[R[C]? B : A] and the
                 * assembly shows it uses cmove and no branches...
                **/
                REG_A() = REG_C()? REG_B() : REG_A();
                DISPATCH_GOTO();
            }

            TARGET(OP_LDA): {
                reg_t b = REG_B(), c = REG_C();
                if(b >= vm.index.size) FAIL(ERR_ARR);

                auto array = vm.index[b];
                if((!array.is_active && b) || c >= array.size) {
                    FAIL(ERR_ARR);
                }

                REG_A() = vm.memory[array.offset + c];
                DISPATCH_GOTO();
            }

            TARGET(OP_STA): {
                reg_t a = REG_A(), b = REG_B();
                if(a >= vm.index.size) FAIL(ERR_ARR);

                auto array = vm.index[a];
                if((!array.is_active && a) || b >= array.size) {
                    FAIL(ERR_ARR);
                }

                vm.memory[array.offset + b] = REG_C();
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
                /*
                reg_t ident = vm.pop_free();

                // Allocate the new array
                reg_t size = REG_C(), offset = vm.memory.size - vm.unused;
                vm.index[ident] = {size, offset};

                // VM spec doesn't specify that `new 0` is invalid, so we'll
                // specially allocate it so it takes up 1 word but is size 0.
                if(size == 0) size = 1;

                REG_B() = ident; // Store the new array index in B
                vm.alloc_memory(size);
                vm.memory.clear(offset, size);
                //printf("NEW %u size %u\n", ident, size);
                //printf("Mem size %u unused %u\n", vm.memory.size, vm.unused);
                DISPATCH_GOTO();
                */

                // VM spec doesn't specify that `new 0` is invalid, so we'll
                // specially allocate it so it takes up 1 word but is size 0.
                reg_t size = REG_C();
                reg_t ident = vm.push_new();
                reg_t offset = vm.memory.size - vm.unused;
                vm.index[ident] = {size, offset};
                vm.alloc_memory(size);
                vm.memory.clear(offset, size);
                REG_B() = ident; // Store the new array index in B
                DISPATCH_GOTO();
            }

            TARGET(OP_DEL): {
                reg_t ident = REG_C();
                if(ident == 0) FAIL(ERR_DEL); // Attempted to delete the program
                if(ident >= vm.index.size) FAIL(ERR_DEL);

                auto array = vm.index[ident];
                if(!array.is_active) FAIL(ERR_DEL); // Inactive array

                vm.shrink_hole(array.offset, array.size);
                vm.push_free(ident);
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
                if(reg_t ident = REG_B()) {
                    if(ident >= vm.index.size) FAIL(ERR_ARR);

                    auto array = vm.index[ident];
                    if(!array.is_active) FAIL(ERR_PRG);

                    if(vm.capacity < array.size) {
                        // Need to grow the memory to fit
                        vm.grow_program(array.size);
                        array = vm.index[ident]; // Refresh
                    }

                    // Assign it
                    vm.progsize = array.size;
                    vm.memory.move(0, array.offset, array.size);
                }
                vm.pc = REG_C();
                DISPATCH_GOTO();
            }

            TARGET(OP_LDI):
                REG_I() = IMM();
                DISPATCH_GOTO();

            TARGET(OP_x14):
            TARGET(OP_x15):
                printf("PC=%u %s\n", vm.pc - 1, opname(cur));
                FAIL(ERR_INV);
        }

        // Fall-through if using switch
        FAIL(ERR_INV);
    } while(vm.pc < vm.progsize);

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
    reg_t size = ftell(fp) / sizeof(reg_t);
    fseek(fp, 0, SEEK_SET);

    reg_t *data = (reg_t *)malloc(size * sizeof(reg_t));

    for(reg_t i = 0; i < size; ++i) {
        uint8_t buf[4];
        if(fread(buf, 1, 4, fp) < 4) {
            break;
        }

        data[i] = buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
    }

    fclose(fp);

    Array<ArrayDef> index(256);
    index[0] = {size, 0}; // Program array

    VM vm = {
        1, // Next free ident is 1
        size, // Capacity of the program array
        size, // Size of the program
        index, // Array index
        0, // Unused memory
        Array<reg_t>(size, data), // Memory
        0, // PC
        {0} // Registers
    };
    vm.set_next(255, 0);

    Error err = interpret(vm);
    if(err) {
        fprintf(stderr, "ERR_%s\n", errname(err));
    }
    return err;
}
