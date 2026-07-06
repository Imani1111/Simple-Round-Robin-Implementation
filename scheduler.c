#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <stdlib.h>

#define MAX_THREADS 2
#define STACK_SIZE 65536
#define STATE_READY   0
#define STATE_RUNNING 1
#define STATE_EXITED  2

typedef struct {
    uint64_t rbx; // Offset 0
    uint64_t rbp; // Offset 8
    uint64_t r12; // Offset 16
    uint64_t r13; // Offset 24
    uint64_t r14; // Offset 32
    uint64_t r15; // Offset 40
    uint64_t rsp; // Offset 48
    uint64_t rip; // Offset 56 Instruction Pointer --> where to resume execution
    int state;    // Offset 64
}Context_t;

Context_t threads[MAX_THREADS];
int current_thread_idx = 0;
void* stack_pool[2];

sigset_t execution_mask;
void thread_exit(void); // Forward declaration

// I want to force the Kernel to redirect the Cpu here after an interrupt 
void timer_interrupt_handler(int signum, siginfo_t* info, void* context)
{
    //TODO: Manual Context Switching to be done here
    ucontext_t *ctx = (ucontext_t*)context;
    threads[current_thread_idx].rsp = ctx->uc_mcontext.gregs[REG_RSP];
    threads[current_thread_idx].rip = ctx->uc_mcontext.gregs[REG_RIP];

    Context_t* current_tcb = &threads[current_thread_idx];

    // 2. Save remaining CPU registers into current TCB using INTEL syntax
    asm volatile(
           ".intel_syntax noprefix\n\t"
           "mov [rax + 0], rbx\n\t"
           "mov [rax + 8], rbp\n\t"
           "mov [rax + 16], r12\n\t"
           "mov [rax + 24], r13\n\t"
           "mov [rax + 32], r14\n\t"
           "mov [rax + 40], r15\n\t"
           ".att_syntax prefix\n\t"
            :
            : "a"(current_tcb)         // Force current_tcb pointer into the RAX register (%0)
            : "memory"
    );

    // 3. System call to unblock the timer so it can tick again
    asm volatile (
        ".intel_syntax noprefix\n\t"
        "mov rax, 14\n\t"      // Syscall 14 = sys_rt_sigprocmask
        "mov rdi, 2\n\t"       // Argument 1: how = SIG_SETMASK (2)
        "mov rsi, %0\n\t"      // Argument 2: set = pointer to execution_mask
        "mov rdx, 0\n\t"       // Argument 3: oldset = NULL
        "mov r10, 8\n\t"       // Argument 4: sigsetsize = 8 bytes
        "syscall\n\t"          // Invoke Kernel
        ".att_syntax prefix\n\t"
        :
        : "r"(&execution_mask)
        : "rax", "rdi", "rsi", "rdx", "r10", "memory"
    );
    write(1, "[Tick]\n", 7);

   //4. Round Robin Task Swap
   current_thread_idx = (current_thread_idx + 1) % 2;
   Context_t* next_tcb = &threads[current_thread_idx];

   // 5. Overwrite the physical CPU with Thread 2's state
    asm volatile (
        ".intel_syntax noprefix\n\t"
        "mov rbx, [rax + 0]\n\t"   // Load RBX from TCB offset 0
        "mov rbp, [rax + 8]\n\t"   // Load RBP from TCB offset 8
        "mov r12, [rax + 16]\n\t"  // Load R12 from TCB offset 16
        "mov r13, [rax + 24]\n\t"  // Load R13 from TCB offset 24
        "mov r14, [rax + 32]\n\t"  // Load R14 from TCB offset 32
        "mov r15, [rax + 40]\n\t"  // Load R15 from TCB offset 40
        
        "mov rsp, [rax + 48]\n\t"  // HIJACK STACK: Load next thread's RSP!
        "jmp qword ptr [rax + 56]\n\t" // HIJACK CODE: Absolute jump to next thread's RIP!
        ".att_syntax prefix\n\t"
        :
        : "a"(next_tcb)            // Force next_tcb pointer into the RAX register
        : "memory"
    );
}

void setup_timer()
{
    sigemptyset(&execution_mask);
    struct sigaction sact;
    // Clear struct memorty
    for (int i =0; i<sizeof(sact);i++){
        ((char*)&sact)[i] = 0;
    }
    // I could also do : [memset(&sa, 0, sizeof(sa));] --> This is used in modern C because its cleaner,
    // faster and can be optimised by compiler easier 
    // but im not using it why? because i feel like using a for loop now 

    sact.sa_sigaction = timer_interrupt_handler;
    sact.sa_flags = SA_SIGINFO | SA_NODEFER;

    //Register action for SIGALRM signal
    sigaction(SIGALRM, &sact, NULL);
}

void start_timer()
{
    struct itimerval timer;
    //This is how long before timer starts
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 150000;
    //This is interval time
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 150000;

    setitimer(ITIMER_REAL, &timer, NULL);
}

void create_thread(int id, void(*thread2_function)(void))
{   //Allocate the stack
    stack_pool[id] = malloc(STACK_SIZE);
    if (!stack_pool[id]) {
        perror("Failed to allocate stack");
        exit(1);
    }

    //Allocate the new thread's rsp and make it a multiple of 16 for x86_64 convection
    uint64_t* top_of_stack = (uint64_t*)(((uint64_t)stack_pool[id] + STACK_SIZE) & 0xFFFFFFFFFFFFFFF0LL);
    top_of_stack[-1] = (uint64_t)thread_exit;

    threads[id].rsp = (uint64_t)&top_of_stack[-1]; // Address contained in the slot is the thread_exit() address
    threads[id].rip = (uint64_t)thread2_function;

    // Clear out general registers so they don't contain garbage
    threads[id].rbx = 0;
    threads[id].rbp = 0;
    threads[id].r12 = 0;
    threads[id].r13 = 0;
    threads[id].r14 = 0;
    threads[id].r15 = 0;
    threads[id].state = STATE_READY;
}

void thread_exit() {
    write(1, "[Sched] A Thread completed lifecycle cleanly.\n", 46);
    // In a multi-threaded system, mark dead and permanently switch back to thread 0
    while(1) {
        // Spin or issue exit syscall
    }
}

void thread_one() {
    uint64_t counter;
    uint64_t sum;
    while(1){
        sum+=counter;
        counter++;
        if ((counter % 10000000) == 0){
            char buf[64];
            int len = snprintf(buf, sizeof(buf), "A: Iteration %lu, Sum = %lu\n", counter, sum);
            write(1, buf, len);
        }
    }
}

void thread_two() {
    uint64_t counter = 0;
    uint64_t product = 1;
    
    while(1) {
        // Do some arbitrary arithmetic calculations to burn cycles
        product = (product + counter) * 3;
        counter++;
        
        if (counter % 10000000 == 0) {
            char buf[64];
            int len = snprintf(buf, sizeof(buf), "     B: Iteration %lu, Hash = %lu\n", counter, product);
            write(1, buf, len);
        }
    } 
}

int main()
{
    printf("[Init] Forging thread stacks and mapping contexts...\n");

    // Populate TCB 0 with thread_one and TCB 1 with thread_two
    create_thread(0, thread_one);
    create_thread(1, thread_two);

    printf("[Init] Arming real-time signal timer. Starting execution loop...\n");
    setup_timer();
    start_timer();

    threads[0].state = STATE_RUNNING;
   
    asm volatile (
        ".intel_syntax noprefix\n\t"
        "mov rsp, [rax + 48]\n\t"
        "jmp qword ptr [rax + 56]\n\t"
        ".att_syntax prefix\n\t"
        :
        : "a"(&threads[0])
        : "memory"
    );
    return 0;
}
