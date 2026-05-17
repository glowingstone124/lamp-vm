#ifndef LAMP_LIBC_TERMIOS_H
#define LAMP_LIBC_TERMIOS_H

#include <lamp/abi.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS LAMP_TERMIOS_NCCS
#define ISIG LAMP_TERMIOS_ISIG
#define ICANON LAMP_TERMIOS_ICANON
#define ECHO LAMP_TERMIOS_ECHO
#define ECHOE 0x00000008u
#define ECHOK 0x00000010u
#define ECHONL 0x00000020u
#define ECHOKE 0x00000800u
#define ECHOPRT 0x00000400u
#define IEXTEN 0x00008000u

/* c_iflag bits */
#define IGNBRK 0x0001
#define BRKINT 0x0002
#define IGNPAR 0x0004
#define PARMRK 0x0008
#define INPCK  0x0010
#define ISTRIP 0x0020
#define INLCR  0x0040
#define IGNCR  0x0080
#define ICRNL  0x0100
#define IUCLC  0x0200
#define IXON   0x0400
#define IXANY  0x0800
#define IXOFF  0x1000
#define IMAXBEL 0x2000
#define IUTF8  0x4000

/* c_oflag bits */
#define OPOST  0x0001
#define OLCUC  0x0002
#define ONLCR  0x0004
#define OCRNL  0x0008
#define ONOCR  0x0010
#define ONLRET 0x0020
#define OFILL  0x0040
#define OFDEL  0x0080
#define NLDLY  0x0100
#define CRDLY  0x0600
#define TABDLY 0x1800
#define BSDLY  0x2000
#define VTDLY  0x4000
#define FFDLY  0x8000

/* c_cflag bits */
#define CBAUD  0x100f
#define CSIZE  0x0030
#define CS5    0x0000
#define CS6    0x0010
#define CS7    0x0020
#define CS8    0x0030
#define CSTOPB 0x0040
#define CREAD  0x0080
#define PARENB 0x0100
#define PARODD 0x0200
#define HUPCL  0x0400
#define CLOCAL 0x0800

/* c_lflag bits */
#define XCASE  0x0004
#define TOSTOP 0x0100
#define FLUSHO 0x1000
#define NOFLSH 0x8000

/* c_cc characters */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int optional_actions, const struct termios *t);
int tcflush(int fd, int queue_selector);

/* Baud rate constants (speed_t values) */
#define B0       0
#define B50      1
#define B75      2
#define B110     3
#define B134     4
#define B150     5
#define B200     6
#define B300     7
#define B600     8
#define B1200    9
#define B1800    10
#define B2400    11
#define B4800    12
#define B9600    13
#define B19200   14
#define B38400   15
#define B57600   0x1001
#define B115200  0x1002
#define B230400  0x1003
#define B460800  0x1004
#define B500000  0x1005
#define B576000  0x1006
#define B921600  0x1007
#define B1000000 0x1008
#define B1152000 0x1009
#define B1500000 0x100a
#define B2000000 0x100b
#define B2500000 0x100c
#define B3000000 0x100d
#define B3500000 0x100e
#define B4000000 0x100f

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2

#endif
