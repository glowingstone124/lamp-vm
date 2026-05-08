# Userspace Bootstrap

Build static user ELFs:

```bash
bash user/build.sh
```

Install the current smoke set into `disk.img`:

```bash
bash user/install_m1_to_disk.sh
```

This requires `debugfs` from e2fsprogs.

Output:

- `build-user/hello.elf`
- `build-user/echo.elf`
- `build-user/vfork_exec.elf`

Current temporary launcher expects the file at ext4 path:

- `/bin/hello`

Install user ELF into ext4 (inside `disk.img` partition at LBA 2048):

```bash
bash user/install_user_to_disk.sh
```

Optional:

- specify input ELF: `--input build-user/hello.elf`
- specify destination path: `--dest /bin/hello`
- patch a raw ext4 image directly: `--rootfs rootfs.ext4`

Install extra M1 test binaries:

```bash
bash user/install_user_to_disk.sh --input build-user/echo.elf --dest /bin/echo
bash user/install_user_to_disk.sh --input build-user/vfork_exec.elf --dest /bin/vfork_exec
```

Or install the full M1 userspace set in one shot:

```bash
bash user/install_m1_to_disk.sh
```

Then from init console run:

- `uhello`
- `uhello 10` to repeat the smoke test 10 times
- `uvfork` (runs `vfork -> execve("/bin/echo") -> waitpid` smoke)
