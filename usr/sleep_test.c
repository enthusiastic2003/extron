#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("Sleep test starting...\n"); 
    fflush(stdout);
    
    for(int i = 0; i < 30; i++) {
        printf("Looping ... %d\n", i);
        fflush(stdout);
        sleep(1);
    }

    printf("Sleep test finished!\n");
    return 0;
}
