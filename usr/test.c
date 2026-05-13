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
        for(int s=1 ; s<=5; s++){
            printf("Child sleeping %d seconds \n", s);
            sleep(1);
        }
        printf("Child woke up\n");
        fflush(stdout);
    }

    printf("Process with fork() ret = %d is exiting.\n", pid);
    return 0;
}