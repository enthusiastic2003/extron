#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv, char **envp) {
    printf("getenv(TERM) = %s\n", getenv("TERM"));
    for (int i=0; envp[i]; i++) {
        printf("envp[%d] = %s\n", i, envp[i]);
    }
    return 0;
}
