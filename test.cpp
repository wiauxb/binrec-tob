#define SYSCALL_RET (ssize_t)({ ssize_t r; __asm__ ("" : "=a"(r) ::); r; })

#define SYSCALL0(nr)         \
    __asm__ __volatile__ (   \
        "movl %0, %%eax\n\t" \
        "int $0x80"          \
        :: "g"(nr)           \
        : "%eax"             \
    )

#define SYSCALL1(nr, arg1)    \
    __asm__ __volatile__ (    \
        "movl %0, %%eax\n\t"  \
        "movl %1, %%ebx\n\t"  \
        "int $0x80"           \
        :: "g"(nr), "g"(arg1) \
        : "%eax", "%ebx"      \
    )

#include <iostream>
#include <syscall.h>
#include <unistd.h>
#include <linux/unistd.h>
#include <asm/ldt.h>
#include <string.h>

ssize_t Func_SYSCALL0(int nr) {
    ssize_t ret;            
    __asm__ __volatile__ (    \
        "movl %1, %%eax\n\t"  \
        "int $0x80"           \
        : "=a"(ret)\
        : "g"(nr)\
        :\
    );          
    return ret; 
}

ssize_t Func_SYSCALL1(int nr, struct user_desc *arg1) {
    ssize_t ret;            
    __asm__ __volatile__ (    \
        "movl %1, %%eax\n\t"  \
        "movl %2, %%ebx\n\t"  \
        "int $0x80"           \
        : "=a"(ret)           \
        : "g"(nr), "g"(arg1)  \
        : "%ebx"              \
    );          
    return ret; 
}

static uint16_t get_gs()
{
    uint16_t gs;
    asm volatile("mov %%gs, %0" : "=g"(gs)::);
    return gs;
}

int main(int argc, char const *argv[])
{
    struct user_desc ds;
    ds.entry_number = get_gs();
    int b = Func_SYSCALL1(SYS_get_thread_area, &ds);
    SYSCALL1(SYS_get_thread_area, &ds);
    int a = SYSCALL_RET;
    std::cout << a << "\n";
    std::cout << b << "\n";
    std::cout << syscall(SYS_get_thread_area, &ds) << " " << strerror(errno) << "\n";
    return 0;
}
