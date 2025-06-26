    #include "../kernel/types.h"
    #include "../kernel/param.h"
    #include "../kernel/memlayout.h"
    #include "../kernel/riscv.h"
    #include "../kernel/spinlock.h"
    #include "../kernel/proc.h" // Assuming proc.h is now needed for thread related structs
    #include "../user/user.h"

    #define STACK_SIZE 100 // A small stack for demonstration purposes

    // This is the function that a new thread will execute.
    void *my_thread(void *arg) {
        // Cast the argument back to uint64, assuming it's a number.
        uint64 number = (uint64)arg;

        // Loop to increment and print the number
        for (int i = 0; i < 100; ++i) {
            number++;
            // Print the thread ID and the current number.
            // Assuming gettid() or similar function is implemented later.
            // For now, just show the number to prove threads are running.
            printf("thread: %lu\n", number);
        }

        // Return the final incremented number.
        return (void *)number;
    }

    int main(int argc, char *argv[]) {
        // Allocate space for thread stacks.
        // Stack grows downwards, so we point to the end of the allocated buffer.
        int sp1[STACK_SIZE], sp2[STACK_SIZE], sp3[STACK_SIZE];

        // Create the first thread.
        // thread(start_thread_function, stack_address, argument)
        int ta = thread(my_thread, sp1 + STACK_SIZE, (void *)100);
        printf("NEW THREAD CREATED %d\n", ta);

        // Create the second thread.
        int tb = thread(my_thread, sp2 + STACK_SIZE, (void *)200);
        printf("NEW THREAD CREATED %d\n", tb);

        // Create the third thread.
        int tc = thread(my_thread, sp3 + STACK_SIZE, (void *)300);
        printf("NEW THREAD CREATED %d\n", tc);

        // Wait for the threads to finish using jointhread.
        jointhread(ta);
        jointhread(tb);
        jointhread(tc);

        printf("DONE\n");

        exit(); // Exit the main process
    }
    