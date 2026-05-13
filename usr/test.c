#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(void) {
    printf("Starting fork test...\n");
    fflush(stdout);
    // sleep(6);

    pid_t pid = fork();

    if (pid < 0) {
        printf("Fork failed! (ret: %d)\n", pid);
    } else if (pid == 0) {
        printf("Hello from the CHILD process!\n");
        // We can do a small delay here if sleep is implemented
        // sleep(1);
    } else {
        printf("Hello from the PARENT process! Child has PID: %d\n", pid);
    }

    if(pid==0){
        printf("Child calling execve(\"./test2\")...\n");
        fflush(stdout);
        char *args[] = {"test2", "hello", "world", "from", "execve", NULL};
        execve("./test2", args, NULL);
        printf("EXECVE FAILED!\n");
    }

    printf("Process with fork() ret = %d is exiting.\n", pid);
    return 0;
}