#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <string.h>

#define TIOCGPTN 0x80045430
#define TIOCSPTLCK 0x40045431

int main() {
    printf("[pty] Opening /dev/ptmx...\n");
    int master = open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        perror("open ptmx");
        return 1;
    }
    
    int pty_num = -1;
    if (ioctl(master, TIOCGPTN, &pty_num) < 0) {
        perror("ioctl TIOCGPTN");
        return 1;
    }
    printf("[pty] Allocated PTY number: %d\n", pty_num);
    
    int zero = 0;
    if (ioctl(master, TIOCSPTLCK, &zero) < 0) {
        perror("ioctl TIOCSPTLCK");
        return 1;
    }
    
    char slave_path[32];
    snprintf(slave_path, sizeof(slave_path), "/dev/pts%d", pty_num);
    printf("[pty] Slave path is %s\n", slave_path);
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    
    if (pid == 0) {
        // Child
        close(master);
        
        // Open slave
        int slave = open(slave_path, O_RDWR);
        if (slave < 0) {
            perror("open slave");
            exit(1);
        }
        
        printf("[pty child] Slave opened successfully (fd %d)\n", slave);
        
        // Replace stdio
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        
        if (slave > 2) close(slave);
        
        printf("Hello from the slave PTY side!\n");
        exit(0);
    } else {
        // Parent
        char buf[128];
        ssize_t n = read(master, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("[pty master] Received from slave: %s\n", buf);
        } else {
            perror("read master");
        }
        
        int status;
        waitpid(pid, &status, 0);
        printf("[pty] Child exited with status %d\n", status);
    }
    return 0;
}
