#ifndef FCNTL_H
#define FCNTL_H
/* Flag values only — nothing opens a file descriptor on this system.
 * DOOM's i_input.c/i_video.c include this for O_* constants in code
 * paths our platform layer replaces. */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_TRUNC    01000
#define O_APPEND   02000
#define O_NONBLOCK 04000
#endif
