#ifndef ARCH_CPU_H
#define ARCH_CPU_H

static inline void cpu_relax(void) {
    __asm__ volatile ("yield");
}

#endif
