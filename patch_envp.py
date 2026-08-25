import sys

# 1. Update exec.h
exec_h = "kernel/include/kernel/proc/exec.h"
with open(exec_h, "r") as f:
    content = f.read()

content = content.replace("#define EXEC_ARG_BYTES 1024", "#define EXEC_ARG_BYTES 1024\n#define EXEC_MAX_ENVS  32\n#define EXEC_ENV_BYTES 2048")
content = content.replace("const char *const *args, int argc,", "const char *const *args, int argc,\n                      const char *const *envp, int envc,")
with open(exec_h, "w") as f:
    f.write(content)

# 2. Update exec.c
exec_c = "kernel/proc/exec.c"
with open(exec_c, "r") as f:
    content = f.read()

content = content.replace("const char *const *args, int argc,", "const char *const *args, int argc,\n                                   const char *const *envp, int envc,")

# Fix _Static_assert
content = content.replace(
    "_Static_assert(EXEC_ARG_BYTES + (EXEC_MAX_ARGS + 3) * sizeof(uint64_t) + 16",
    "_Static_assert(EXEC_ARG_BYTES + EXEC_ENV_BYTES + (EXEC_MAX_ARGS + EXEC_MAX_ENVS + 3) * sizeof(uint64_t) + 16"
)

old_build_stack = """static virt_addr_t build_arg_stack(phys_addr_t top_phys,
                                   const char *const *args, int argc,
                                   virt_addr_t *out_argv_va) {
    uint8_t     *page    = (uint8_t *)phys_to_virt_hhdm(top_phys);
    virt_addr_t  page_va = USER_STACK_TOP - PAGE_SIZE;
    virt_addr_t  str_va[EXEC_MAX_ARGS];
    size_t       off = PAGE_SIZE;

    /* Strings first, from the top down, so the pointer array below them
     * can be filled in with addresses that are already final. */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(args[i]) + 1;
        off -= len;
        memcpy(page + off, args[i], len);
        str_va[i] = page_va + off;
    }

    off &= ~(size_t)7;
    /* argc's own word, argc argv pointers, argv's NULL terminator,
     * envp's (empty) NULL terminator. */
    off -= (size_t)(argc + 3) * sizeof(uint64_t);
    off &= ~(size_t)15;             /* AAPCS64: sp must be 16-byte aligned */

    uint64_t *sp_words = (uint64_t *)(page + off);
    sp_words[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        sp_words[1 + i] = str_va[i];
    sp_words[1 + argc]     = 0;     /* argv[argc] == NULL, as C requires */
    sp_words[1 + argc + 1] = 0;     /* envp[0] == NULL: no environment yet */

    /* x1 (our own crt0's argv register) points at the argv array
     * specifically — one word past argc — not at sp itself. */
    *out_argv_va = page_va + off + sizeof(uint64_t);
    return page_va + off;           /* sp: argc, then argv[], as mlibc's
                                      * init_libc() constructor expects */
}"""

new_build_stack = """static virt_addr_t build_arg_stack(phys_addr_t top_phys,
                                   const char *const *args, int argc,
                                   const char *const *envp, int envc,
                                   virt_addr_t *out_argv_va) {
    uint8_t     *page    = (uint8_t *)phys_to_virt_hhdm(top_phys);
    virt_addr_t  page_va = USER_STACK_TOP - PAGE_SIZE;
    virt_addr_t  str_va[EXEC_MAX_ARGS];
    virt_addr_t  env_va[EXEC_MAX_ENVS];
    size_t       off = PAGE_SIZE;

    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        off -= len;
        memcpy(page + off, envp[i], len);
        env_va[i] = page_va + off;
    }

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(args[i]) + 1;
        off -= len;
        memcpy(page + off, args[i], len);
        str_va[i] = page_va + off;
    }

    off &= ~(size_t)7;
    off -= (size_t)(argc + envc + 3) * sizeof(uint64_t);
    off &= ~(size_t)15;             /* AAPCS64: sp must be 16-byte aligned */

    uint64_t *sp_words = (uint64_t *)(page + off);
    sp_words[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        sp_words[1 + i] = str_va[i];
    sp_words[1 + argc] = 0;
    
    for (int i = 0; i < envc; i++)
        sp_words[1 + argc + 1 + i] = env_va[i];
    sp_words[1 + argc + 1 + envc] = 0;

    *out_argv_va = page_va + off + sizeof(uint64_t);
    return page_va + off;
}"""
content = content.replace(old_build_stack, new_build_stack)

old_exec_image_build = """static int exec_image_build(struct proc *requester, const char *binary_path,
                            const char *const *args, int argc,
                            struct exec_image *out) {"""
new_exec_image_build = """static int exec_image_build(struct proc *requester, const char *binary_path,
                            const char *const *args, int argc,
                            const char *const *envp, int envc,
                            struct exec_image *out) {"""
content = content.replace(old_exec_image_build, new_exec_image_build)

content = content.replace("out->user_sp = build_arg_stack(top_phys, args, argc, &out->argv);",
                          "out->user_sp = build_arg_stack(top_phys, args, argc, envp, envc, &out->argv);")

old_proc_exec = """int proc_exec_replace(struct proc *p, const char *binary_path,
                      const char *const *args, int argc,
                      struct exec_image *out) {
    return exec_image_build(p, binary_path, args, argc, out);
}"""
new_proc_exec = """int proc_exec_replace(struct proc *p, const char *binary_path,
                      const char *const *args, int argc,
                      const char *const *envp, int envc,
                      struct exec_image *out) {
    return exec_image_build(p, binary_path, args, argc, envp, envc, out);
}"""
content = content.replace(old_proc_exec, new_proc_exec)

content = content.replace("if (exec_image_build(NULL, binary_path, args, argc, &img) != 0)",
                          "if (exec_image_build(NULL, binary_path, args, argc, NULL, 0, &img) != 0)")

with open(exec_c, "w") as f:
    f.write(content)

# 3. Update syscall.c
syscall_c = "kernel/proc/syscall.c"
with open(syscall_c, "r") as f:
    content = f.read()

old_sys_execve_gather = """    char        argbuf[EXEC_ARG_BYTES];
    const char *args[EXEC_MAX_ARGS];
    int         argc = 0;
    size_t      used = 0;

    if (argv_addr) {
        for (;;) {
            if (argc >= EXEC_MAX_ARGS) {
                kprintf("[SYSCALL execve] too many arguments (max %d)\\n",
                        EXEC_MAX_ARGS);
                return (uint64_t)-1;
            }
            uint64_t slot = argv_addr + (uint64_t)argc * sizeof(uint64_t);
            if (!user_buffer_ok(p, slot, sizeof(uint64_t)))
                return (uint64_t)-1;
            uint64_t str = *(const uint64_t *)slot;
            if (!str)
                break;                       /* argv[argc] == NULL */

            long len = copy_user_string(p, str, argbuf + used,
                                        EXEC_ARG_BYTES - used);
            if (len < 0) {
                kprintf("[SYSCALL execve] argv[%d] unreadable or too long\\n", argc);
                return (uint64_t)-1;
            }
            args[argc++] = argbuf + used;
            used += (size_t)len + 1;
        }
    }

    /* An argv with no argv[0] is legal to pass and useless to receive.
     * Substitute the path, which is what the process would report as its
     * own name anyway. */
    if (argc == 0) {
        args[0] = path;
        argc = 1;
    }

    struct exec_image img;
    if (proc_exec_replace(p, path, args, argc, &img) != 0)
        return (uint64_t)-1;"""

new_sys_execve_gather = """    char        argbuf[EXEC_ARG_BYTES];
    const char *args[EXEC_MAX_ARGS];
    int         argc = 0;
    size_t      used = 0;

    if (argv_addr) {
        for (;;) {
            if (argc >= EXEC_MAX_ARGS) {
                kprintf("[SYSCALL execve] too many arguments (max %d)\\n",
                        EXEC_MAX_ARGS);
                return (uint64_t)-1;
            }
            uint64_t slot = argv_addr + (uint64_t)argc * sizeof(uint64_t);
            if (!user_buffer_ok(p, slot, sizeof(uint64_t)))
                return (uint64_t)-1;
            uint64_t str = *(const uint64_t *)slot;
            if (!str)
                break;                       /* argv[argc] == NULL */

            long len = copy_user_string(p, str, argbuf + used,
                                        EXEC_ARG_BYTES - used);
            if (len < 0) {
                kprintf("[SYSCALL execve] argv[%d] unreadable or too long\\n", argc);
                return (uint64_t)-1;
            }
            args[argc++] = argbuf + used;
            used += (size_t)len + 1;
        }
    }

    if (argc == 0) {
        args[0] = path;
        argc = 1;
    }

    char        envbuf[EXEC_ENV_BYTES];
    const char *envs[EXEC_MAX_ENVS];
    int         envc = 0;
    size_t      env_used = 0;

    if (envp_addr) {
        for (;;) {
            if (envc >= EXEC_MAX_ENVS) {
                kprintf("[SYSCALL execve] too many envs (max %d)\\n", EXEC_MAX_ENVS);
                return (uint64_t)-1;
            }
            uint64_t slot = envp_addr + (uint64_t)envc * sizeof(uint64_t);
            if (!user_buffer_ok(p, slot, sizeof(uint64_t)))
                return (uint64_t)-1;
            uint64_t str = *(const uint64_t *)slot;
            if (!str)
                break;

            long len = copy_user_string(p, str, envbuf + env_used,
                                        EXEC_ENV_BYTES - env_used);
            if (len < 0) {
                kprintf("[SYSCALL execve] envp[%d] unreadable or too long\\n", envc);
                return (uint64_t)-1;
            }
            envs[envc++] = envbuf + env_used;
            env_used += (size_t)len + 1;
        }
    }

    struct exec_image img;
    if (proc_exec_replace(p, path, args, argc, envs, envc, &img) != 0)
        return (uint64_t)-1;"""
content = content.replace(old_sys_execve_gather, new_sys_execve_gather)
with open(syscall_c, "w") as f:
    f.write(content)

print("Patched envp support")
