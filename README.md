# Custom Preemptive User-Space Thread Scheduler

A low-level, user-space preemptive multitasking kernel engine written entirely from scratch in C and pure Intel x86_64 assembly. This project serves as an educational blueprint demonstrating how operating system kernels manage thread lifecycles, manipulate physical hardware registers, isolate stack topologies, resolve pointer arithmetic boundaries, and execute asynchronous context switches without relying on standard POSIX threading libraries (`pthread`).

---

## 1. Core Core Principles & The Scheduling Illusion

### Shifting from Cooperative to Preemptive Multi-Tasking
In a **cooperative scheduler**, threads must voluntarily yield control back to the operating system using explicit functions (like `sleep()` or `yield()`). If a thread enters an infinite computation loop, it starves the rest of the machine. 

This engine implements **preemptive scheduling**, where execution tasks are forcefully yanked away from threads at regular time intervals without their explicit permission, awareness, or cooperation.

### The Virtualization of Time (Multi-Level Preemption)
Because user-space applications are blocked by the CPU architecture from touching raw hardware timer interrupts directly, this project virtualizes hardware interrupts by utilizing the Linux kernel's real-time interval timer infrastructure (`setitimer`). 

When this application executes on a single-core computer, a multi-layered illusion occurs:
1. **The Global OS Level (Linux):** The Linux kernel treats this entire compiled binary as just one single task among thousands of others on the system. Linux periodically freezes our process via its own kernel scheduling ticks to let other apps (like a web browser or terminal) run.
2. **The Local App Level (Our Scheduler):** When Linux hands control to our process, our engine acts as a micro-kernel. It intercepts the virtual hardware timer (`SIGALRM`) every 50ms, using inline assembly to slice up its given time quantum between our internal mathematical worker threads.
3. **The Hardware Core:** The physical CPU core simply executes instructions sequentially, entirely unaware of the nested layers of virtualization shifting its internal registers.

---

## 2. Structural Layout: Thread Control Block (TCB)

To switch execution between isolated tasks, a CPU must have a reliable parking lot in RAM to store a thread's state before its registers are overwritten by the next task. This is achieved via our custom **Thread Control Block** (`Context_t`).

Each thread is allocated a persistent, 64-bit aligned block in memory mapped out to the following exact structural byte offsets:

| Register | TCB Struct Field | Offset (Bytes) | Architectural Function |
| :--- | :--- | :--- | :--- |
| `RBX` | `rbx` | `0` | Callee-saved general purpose register |
| `RBP` | `rbp` | `8` | Frame pointer (Tracks local variable stack boundaries) |
| `R12` | `r12` | `16` | Callee-saved general purpose register |
| `R13` | `r13` | `24` | Callee-saved general purpose register |
| `R14` | `r14` | `32` | Callee-saved general purpose register |
| `R15` | `r15` | `40` | Callee-saved general purpose register |
| `RSP` | `rsp` | `48` | Stack Pointer (Points to the active top of the thread stack) |
| `RIP` | `rip` | `56` | Instruction Pointer / Program Counter (Where to resume execution) |

---

## 3. Deep Dive: Low-Level Execution Mechanics

### A. Thread Birth & Stack Forging
When a thread is generated via `create_thread`, raw heap memory is allocated using `malloc`. 

1. **Downward Growth:** On x86_64 architectures, physical stack memory grows downward (from high memory addresses to lower memory addresses). Therefore, the initial Stack Pointer (`RSP`) must be calculated from the absolute top boundary of the allocated block (`stack_pool[id] + STACK_SIZE`).
2. **16-Byte Alignment:** The System V AMD64 ABI calling convention mandates that the stack pointer must be aligned to a 16-byte boundary before any function call. We enforce this mathematically using a bitwise AND mask: `& 0xFFFFFFFFFFFFFFF0LL`.
3. **The Trampoline Net:** The address of a lifecycle cleanup function (`thread_exit`) is explicitly written into the baseline cell of the stack frame (`top_of_stack[-1]`). If a thread finishes its work loop and reaches its closing bracket, the CPU naturally pops this address via the standard assembly `ret` instruction. This safely redirects the thread into a controlled spinlock instead of letting it drop into uninitialized memory and causing a Segmentation Fault.
4. **Pointer Address Fix:** When saving the initial stack pointer inside the TCB, we use the address-of operator (`&top_of_stack[-1]`). This registers the **memory address of the stack slot itself**, rather than copying the value stored inside that slot, ensuring `RSP` points to writeable RAM instead of read-only code sections.

### B. The Context Heist (Save Phase)
When the recurring timer ticks down, the Linux kernel forcefully halts our active thread and switches execution into `timer_interrupt_handler`.

1. The kernel packs the physical registers of the interrupted thread into a `ucontext_t` structure passed as a pointer inside the handler arguments.
2. The scheduler immediately pulls the live `RIP` and `RSP` from the kernel context frame array indices (`REG_RIP` and `REG_RSP`). This automatically bypasses Address Space Layout Randomization (ASLR) and Position-Independent Executable (PIE) protections because we are capturing live runtime memory addresses.
3. We drop into naked inline assembly using **Intel Syntax** to capture the remaining active CPU registers and drop them into the exact memory offsets of our active TCB:
   ```assembly
   mov [rax + 0], rbx    ; Move physical RBX into TCB offset 0
   mov [rax + 8], rbp    ; Move physical RBP into TCB offset 8
   ...
