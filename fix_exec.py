import sys

exec_c = "kernel/proc/exec.c"
with open(exec_c, "r") as f:
    content = f.read()

# Fix proc_exec_replace
old_proc_exec = """int proc_exec_replace(struct proc *p, const char *binary_path,
                                   const char *const *args, int argc,
                                   const char *const *envp, int envc,
                                   struct exec_image *out) {
    if (exec_image_build(p, binary_path, args, argc, out) != 0)
        return -1;"""
new_proc_exec = """int proc_exec_replace(struct proc *p, const char *binary_path,
                                   const char *const *args, int argc,
                                   const char *const *envp, int envc,
                                   struct exec_image *out) {
    if (exec_image_build(p, binary_path, args, argc, envp, envc, out) != 0)
        return -1;"""
content = content.replace(old_proc_exec, new_proc_exec)

with open(exec_c, "w") as f:
    f.write(content)
