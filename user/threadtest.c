#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/spinlock.h"
#include "kernel/proc.h"
#include "user/user.h" // Includes thread() and jointhread() prototypes

#define STACK_SIZE 100 // Define stack size for threads

// Thread function that will be executed by new threads
// Corrected: Changed return type from 'void' to 'void *'
void *my_thread(void *arg) {
    uint64 number = (uint64)arg;
    for (int i = 0; i < 100; ++i) {
        number++;
        printf("thread %lu: %d\n", number, i); // Use %lu for uint64
    }
    return (void*)number; // This return is now valid
}

int main(int argc, char *argv[]) {
    // Allocate stack memory for threads
    // Each thread needs its own user-space stack.
    // For a real scenario, these should be dynamically allocated pages (e.g., using sbrk).
    // Here, we use static arrays for simplicity, assuming STACK_SIZE is sufficient.
    char sp1[STACK_SIZE], sp2[STACK_SIZE], sp3[STACK_SIZE];

    printf("Main thread: Creating new threads...\n");

    // Create threads
    // thread(function_pointer, stack_pointer, argument)
    // Note: (int*)sp1 is a cast for the stack pointer argument.
    // The argument 100, 200, 300 are cast to (void*) for the thread function.
    int ta = thread(my_thread, (int*)sp1, (void*)100);
    printf("NEW THREAD CREATED %d\n", ta); // Thread ID returned by thread()

    int tb = thread(my_thread, (int*)sp2, (void*)200);
    printf("NEW THREAD CREATED %d\n", tb);

    int tc = thread(my_thread, (int*)sp3, (void*)300);
    printf("NEW THREAD CREATED %d\n", tc);

    // Join threads (wait for them to finish)
    printf("Main thread: Joining threads...\n");
    jointhread(ta);
    jointhread(tb);
    jointhread(tc);

    printf("DONE\n"); // All threads finished

    exit(0); // Exit the main process
}