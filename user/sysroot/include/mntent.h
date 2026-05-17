#ifndef LAMP_LIBC_MNTENT_H
#define LAMP_LIBC_MNTENT_H

struct mntent { char *mnt_fsname; char *mnt_dir; char *mnt_type; char *mnt_opts; int mnt_freq; int mnt_passno; };

#endif
