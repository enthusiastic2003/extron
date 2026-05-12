#include <stdio.h>

int main(void) {
    char buf[128];

    printf("Enter something: ");
    fflush(stdout);

    if(fgets(buf, sizeof(buf), stdin)) {
        printf("You entered: %s", buf);
    } else {
        printf("Input failed\n");
    }

    return 0;
}