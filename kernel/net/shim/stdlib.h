/* lwIP's netif.c wants atoi(): provided by kernel/net/net.c. */
#ifndef NET_SHIM_STDLIB_H
#define NET_SHIM_STDLIB_H
int atoi(const char *s);
#endif
