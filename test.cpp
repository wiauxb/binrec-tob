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
#include <sys/ptrace.h>


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

int old_main(int argc, char const *argv[])
{
    struct user_desc ds;
    ds.entry_number = get_gs();
    int b = Func_SYSCALL1(SYS_get_thread_area, &ds);
    SYSCALL1(SYS_get_thread_area, &ds);
    int a = SYSCALL_RET;
    std::cout << ds.entry_number << "\n";
    std::cout << a << "\n";
    std::cout << b << "\n";
    
    struct user_desc dsi;
    dsi.entry_number = (unsigned int) -1;
    SYSCALL1(SYS_set_thread_area, &dsi);
    // SYSCALL1(SYS_get_thread_area, &dsi);
    unsigned int c = dsi.entry_number;
    std::cout << c << "\n";
    return 0;
}

int main(int argc, char const *argv[])
{
    struct user_desc ds;
    int gs = get_gs();
    pid_t pid = syscall(__NR_gettid);
    std::cout << pid << " " << getpid() << " " << pthread_self() << " " << strerror(errno) << "\n";
    for (int i = gs; i <= gs; i++)
    {
        std::cout << ptrace(PTRACE_GET_THREAD_AREA, pthread_self(), i, &ds) << " " << i  << " " << strerror(errno) << "\n";
        // print_user_desc(&ds);
    }
    
    return 0;
}
