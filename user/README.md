# Userspace Bootstrap

Build static user hello ELF:

```bash
bash user/build.sh
```

Output:

- `build-user/hello.elf`

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

Then from init console run:

- `uhello`
- `uhello 10` to repeat the smoke test 10 times
