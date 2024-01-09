#include "binrec/rt/cpu_x86.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <asm/ldt.h>
#include <linux/unistd.h>
#include <sys/syscall.h>
#include <iostream>
#include <unistd.h>


extern "C" {
#include <asm/ldt.h>
#include <linux/unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include "mysyscall.h"
#include <errno.h>
#include <string.h>

typedef target_ulong addr_t;
typedef addr_t stackword_t;

// defined in llvm module
extern uint32_t debug_verbosity;

extern SegmentCache segs[6];

extern void Func_wrapper(void);
// This comes with captured.bc. Originially implemented in op_helper.c. op_helper.bc is already
// linked with captured.bc in s2e.
extern void helper_fninit(void);
extern void helper_fldl_ST0(uint64_t);
extern void helper_flds_ST0(uint32_t);

typedef unsigned uint;

void raise_interrupt(int intno, int is_int, int error_code, int next_eip_addend)
{
    printf(
        "interrupt intno=%x, is_int=%x, error_code=%x, next_eip_addend=%x\n",
        intno,
        is_int,
        error_code,
        next_eip_addend);
    exit(-1);
}

void raise_interrupt2(int intno, int is_int, int error_code, int next_eip_addend)
{
    raise_interrupt(intno, is_int, error_code, next_eip_addend);
}

void __attribute__((noinline)) helper_raise_exception(uint32_t index)
{
    printf("exception %d raised\n", index);
    exit(-1);
}

static char segmem[1024];

static uint16_t get_gs()
{
    uint16_t gs;
    asm volatile("mov %%gs, %0" : "=g"(gs)::);
    return gs;
}

static void print_user_desc(struct user_desc* ud){
    printf("entry_number %d\n", ud->entry_number);
    printf("base_addr %d\n", ud->base_addr);
    printf("limit %d\n", ud->limit);
    printf("seg_32bit %d\n", ud->seg_32bit);
    printf("contents %d\n", ud->contents);
    printf("read_exec_only %d\n", ud->read_exec_only);
    printf("limit_in_pages %d\n", ud->limit_in_pages);
    printf("seg_not_present %d\n", ud->seg_not_present);
    printf("useable %d\n", ud->useable);
}

// static unsigned int init_new_tls(struct user_desc * ud){
//     std::cout << "Help\n";
//     struct user_desc dsi;
//     dsi.entry_number = -1;
//     syscall(SYS_set_thread_area, &dsi);
//     syscall(SYS_get_thread_area, &dsi);
//     unsigned int c = dsi.base_addr;
//     std::cout << c << "\n";
//     std::cout << dsi.entry_number << dsi.base_addr << "\n";
//     return c;
// }


static void init_gs(SegmentCache *ds)
{
    // struct user_desc info; 
    // init_new_tls(&info);
    ds->selector = get_gs();
    syscall(SYS_get_thread_area, (struct user_desc*)ds);
    ssize_t ret = SYSCALL_RET;  // FIXME: THIS KEEPS FAILING!!!???
    printf("%d\n", ret);
    struct user_desc info = {.entry_number = get_gs()};
    syscall(SYS_get_thread_area, &info);
    ret = SYSCALL_RET;  // FIXME: THIS KEEPS FAILING!!!???
    printf("%d\n", ret);
    print_user_desc(&info);
    printf("----------------------------\n");
    print_user_desc((struct user_desc*)ds);

    // if (ret) {
    // //    binrecrt_fdprintf(STDERR_FILENO, "get_thread_area returned ");
    // //    binrecrt_write_dec(DEBUG_FD, ret);
    // //    binrecrt_write_char(DEBUG_FD, '\n');
    // //    binrecrt_exit(-2);
    //     printf("get_thread_area returned %d\n", ret);
    //     printf("errno: %d = %s\n", errno, strerror(errno));
    //     // exit(-2);
    // }

    // ds->base = info.base_addr;

    // for now just emulate the TLS by setting the base pointer to a global
    // array so that accesses do not segfault (note that the stack canary will
    // always be zero)
    ds->base = (target_ulong)segmem;
}

static void init_env()
{
    reg_t eflags;
    asm("pushf\n\t"
        "popl %0"
        : "=r"(eflags)::);

    mflags = eflags & MFLAGS_MASK;
    df = eflags & DF_MASK ? -1 : 1;
}

void __attribute__((always_inline)) nonlib_setjmp()
{

    addr_t retaddr = helper_ldl_mmu(nullptr, R_ESP, 0, nullptr);
    R_ESP += sizeof(stackword_t);

    reg_t *r_esp = (reg_t *)R_ESP;
    unsigned long *buf = (unsigned long *)*r_esp;
    buf[0] = R_EBX;
    buf[1] = R_ESI;
    buf[2] = R_EDI;
    buf[3] = R_EBP;
    buf[4] = R_ESP;
    buf[5] = retaddr;
    R_EAX = 0;
    PC = retaddr;
}

void __attribute__((always_inline)) nonlib_longjmp()
{

    // pop ret address, we don't need it
    // addr_t retaddr = __ldl_mmu(R_ESP, 0);
    R_ESP += sizeof(stackword_t);
    // get first param(buffer)
    reg_t *r_esp = (reg_t *)R_ESP;
    unsigned long *buf = (unsigned long *)*r_esp;
    // get second param(ret val for setjmp)
    R_ESP += sizeof(stackword_t);
    r_esp = (reg_t *)R_ESP;
    R_EAX = *r_esp;

    if (!R_EAX)
        R_EAX++;
    R_EBX = buf[0];
    R_ESI = buf[1];
    R_EDI = buf[2];
    R_EBP = buf[3];
    R_ESP = buf[4];
    PC = buf[5];
}

uint64_t __attribute__((noinline, fastcall))
helper_stub_trampoline(const reg_t ecx, const reg_t edx, const reg_t esp, const addr_t targetpc)
{
    // Note(FPar): We can probably remove edx and ecx here, because they are not preserved and
    // should not be arguments to any standard library function (unless they use fastcall, too?)

    // see
    // https://developer.apple.com/library/mac/documentation/DeveloperTools/Conceptual/LowLevelABI/130-IA-32_Function_Calling_Conventions/IA32.html

    // This struct is optimized away by the compiler because the return value
    // of the 'call' below is returned in edx/eax and then also used as return
    // value for this helper. Its purpose is to make the type checker happy,
    // since now we can return the struct as a value instead of not returning
    // anything (isn't it beautiful?)
    struct {
        reg_t eax;
        reg_t edx;
    } ret;

    // set stack pointer and call library function
    asm("movl    %%esp, %%ebx\n\t"
        "movl    %2, %%esp\n\t"
        "calll   *%3\n\t"
        "movl    %%ebx, %%esp"
        : "=a"(ret.eax), "=d"(ret.edx)
        : "r"(esp), "r"(targetpc), "c"(ecx), "d"(edx)
        : "ebx");

    double val;

    // Known limitation: this always converts 80bit floating point values
    // to 64-bit. We do this because the Qemu 80bit helper functions are
    // not available so we have to use the 64-bit functions.
    //
    // See https://github.com/trailofbits/binrec-prerelease/issues/187
    // for more information.
    asm("fstpl   %0" : "=m"(val)::);
    helper_fldl_ST0(val);

    // this is optimized away, as explained above
    return ((uint64_t)ret.edx << 32) | ret.eax;
}

void __attribute__((always_inline)) helper_extern_stub()
{
    // return address should be on top of virtual stack, pop it
    addr_t retaddr =helper_ldl_mmu(nullptr, R_ESP, 0, nullptr);
    R_ESP += sizeof(stackword_t);

    // PC should contain the address of the target function, call it
    uint64_t ret = helper_stub_trampoline(R_ECX, R_EDX, R_ESP, PC);

    // copy return value fo library function to virtual environment
    R_EAX = ret & 0xffffffff;
    R_EDX = ret >> 32;

    // jump to the return address
    PC = retaddr;
}

void __attribute__((always_inline)) virtualize_return_float(float ret)
{
    uint32_t *ret32 = (uint32_t*)&ret;
    helper_flds_ST0(*ret32);
}

void __attribute__((always_inline)) virtualize_return_double(double ret)
{
    uint64_t *ret64 = (uint64_t*)&ret;
    helper_fldl_ST0(*ret64);
}

void __attribute__((always_inline)) virtualize_return_i32(uint32_t ret)
{
    R_EAX = ret;
}

void __attribute__((always_inline)) virtualize_return_i64(uint64_t ret)
{
    R_EAX = ret & 0xffffffff;
    R_EDX = ret >> 32;
}

/* ABI for atexit() function. Binrec does not currently support callbacks, so we remove
 * this functionality. */
int __cxa_atexit(void *, void*, void*) {
    return 0;
}

void helper_break()
{
    asm("int $0x3");
}

// redefine the following functions to avoid cases that handle symbolic memory
// in the default implementations
//
// TODO(meily) - I don't think these functions exist anywhere in the updated S2E. There
// are "cpu_*_data" functions that I think these *could* replace. However, with the
// work on issue #101, https://github.com/trailofbits/binrec-prerelease/issues/101,
// I don't think these are actually necessary and can be removed.
uint64_t __attribute__((always_inline)) ldq_data(uint32_t ptr)
{
    return helper_ldq_mmu(nullptr, ptr, 0, nullptr);
}

uint32_t __attribute__((always_inline)) lduw_data(uint32_t ptr)
{
    return helper_ldw_mmu(nullptr, ptr, 0, nullptr);
}

void __attribute__((always_inline)) stq_data(uint32_t ptr, uint64_t value)
{
    helper_stq_mmu(nullptr, ptr, value, 0, nullptr);
}

void __attribute__((always_inline)) stw_data(uint32_t ptr, uint32_t value)
{
    helper_stw_mmu(nullptr, ptr, value, 0, nullptr);
}

void helper_s2e_tcg_custom_instruction_handler(uint32_t opcode)
{
    printf("custom s2e instruction with opcode %x called\n", opcode);
}
}

int main(int argc, char **argv, char **envp)
{
    assert(sizeof(stackword_t) == sizeof(void *));

    if (debug_verbosity >= 1) {
        printf("stack-begins at = %p\n", &stack[STACK_SIZE - 1]);
    }

    init_env();
    init_gs(&segs[R_GS]);
    helper_fninit();

    stack[STACK_SIZE - 1] = (stackword_t)envp;
    stack[STACK_SIZE - 2] = (stackword_t)argv;
    stack[STACK_SIZE - 3] = (stackword_t)argc;
    stack[STACK_SIZE - 4] = (stackword_t)__builtin_return_address(0);

    R_ESP = (stackword_t)&stack[STACK_SIZE - 4];

    Func_wrapper();

    if (debug_verbosity >= 1) {
        puts("end of custom main");
    }

    return (int)R_EAX;
}
