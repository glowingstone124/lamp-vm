#ifndef LAMP_LIBC_SIGNAL_H
#define LAMP_LIBC_SIGNAL_H

#include <lamp/abi.h>
#include <sys/types.h>

typedef unsigned int sigset_t;
typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#define SIG_DFL ((sighandler_t)LAMP_SIG_DFL)
#define SIG_IGN ((sighandler_t)LAMP_SIG_IGN)
#define SIG_ERR ((sighandler_t)-1)
#define SIGHUP LAMP_SIGHUP
#define SIGINT LAMP_SIGINT
#define SIGQUIT LAMP_SIGQUIT
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL LAMP_SIGKILL
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM LAMP_SIGTERM
#define SIGCHLD LAMP_SIGCHLD
#define SIGCONT 18
#define SIGSTOP LAMP_SIGSTOP
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIG_BLOCK LAMP_SIG_BLOCK
#define SIG_UNBLOCK LAMP_SIG_UNBLOCK
#define SIG_SETMASK LAMP_SIG_SETMASK

#define SA_RESTART 0x00000004u
#define NSIG 65

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int kill(pid_t pid, int sig);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
int sigisemptyset(const sigset_t *set);
sighandler_t signal(int sig, sighandler_t handler);
int sigsuspend(const sigset_t *mask);
int raise(int sig);

#endif
