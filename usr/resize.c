#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <string.h>

int main() {
    struct termios old, new;
    tcgetattr(0, &old);
    new = old;
    new.c_lflag &= ~(ICANON | ECHO);
    new.c_cc[VMIN] = 1;
    new.c_cc[VTIME] = 1;
    tcsetattr(0, TCSANOW, &new);

    // Save cursor position
    printf("\0337");
    // Move cursor to bottom-right (999, 999)
    printf("\033[999;999H");
    // Device Status Report - Query cursor position
    printf("\033[6n");
    fflush(stdout);

    char buf[32];
    int idx = 0;
    while (idx < 31) {
        if (read(0, &buf[idx], 1) != 1) break;
        if (buf[idx] == 'R') {
            idx++;
            break;
        }
        idx++;
    }
    buf[idx] = '\0';

    // Restore cursor position
    printf("\0338");
    fflush(stdout);

    tcsetattr(0, TCSANOW, &old);

    int row = 0, col = 0;
    if (sscanf(buf, "\033[%d;%dR", &row, &col) == 2) {
        struct winsize {
            unsigned short ws_row;
            unsigned short ws_col;
            unsigned short ws_xpixel;
            unsigned short ws_ypixel;
        } ws;
        ws.ws_row = row;
        ws.ws_col = col;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        
        if (ioctl(0, 0x5414, &ws) == 0) { // TIOCSWINSZ
            printf("Terminal size auto-detected and set to %d rows, %d columns.\n", row, col);
            return 0;
        } else {
            printf("Failed to set terminal size.\n");
            return 1;
        }
    }
    
    printf("Failed to read terminal size (got '%s').\n", buf);
    return 1;
}
