#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char buffer[256];

    printf("Input test starting...\n");
    printf("Received %d arguments:\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = '%s'\n", i, argv[i]);
    }

    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(buffer, sizeof(buffer), stdin)) {
            break;
        }

        buffer[strcspn(buffer, "\n")] = '\0';

        if (strcmp(buffer, "exit") == 0) {
            break;
        }

        printf("You entered: %s\n", buffer);
    }

    printf("Input test finished!\n");
    return 0;
}