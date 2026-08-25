import sys

task_md = "/home/sirjanh/.gemini/antigravity/brain/4e7322ca-3e1f-4cf9-8b02-27a3426b7623/task.md"
with open(task_md, "r") as f:
    content = f.read()

content = content.replace("[ ] Refactor `tty.c` to support multiple `struct tty` instances.", "[x] Refactor `tty.c` to support multiple `struct tty` instances.")
content = content.replace("[ ] Implement PTY Master/Slave endpoints in `devfs.c` and `sys_open`.", "[x] Implement PTY Master/Slave endpoints in `devfs.c` and `sys_open`.")
content = content.replace("[ ] Link `mlibc` PTY sysdeps (`TIOCGPTN`, `TIOCSPTLCK`).", "[x] Link `mlibc` PTY sysdeps (`TIOCGPTN`, `TIOCSPTLCK`).")
content = content.replace("[ ] Test `posix_openpt()` and `grantpt()` inside Extron.", "[x] Test `posix_openpt()` and `grantpt()` inside Extron.")

with open(task_md, "w") as f:
    f.write(content)

