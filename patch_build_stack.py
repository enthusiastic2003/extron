import sys

exec_c = "kernel/proc/exec.c"
with open(exec_c, "r") as f:
    content = f.read()

# Find the start and end of build_arg_stack
start_str = "static virt_addr_t build_arg_stack"
start_idx = content.find(start_str)
end_str = "}\n\n/*\n * Reads the whole of `binary_path`"
end_idx = content.find(end_str) + 1

if start_idx != -1 and end_idx != -1:
    new_func = """static virt_addr_t build_arg_stack(phys_addr_t top_phys,
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
    content = content[:start_idx] + new_func + content[end_idx:]
    with open(exec_c, "w") as f:
        f.write(content)
    print("Patched build_arg_stack")
else:
    print("Could not find boundaries")
