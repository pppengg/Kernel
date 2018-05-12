/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <stdarg.h>
#include <time.h>

#include <asm/system.h>
#include <asm/io.h>

#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/head.h>
#include <linux/unistd.h>

#ifdef CONFIG_TESTCASE
#include <test/debug.h>
#endif

/*
 * we need this inline - forking for kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
inline int fork(void) __attribute__((always_inline));
inline int pause(void) __attribute__((always_inline));
inline _syscall0(int, fork)
inline _syscall1(int, setup, void *, BIOS)
inline _syscall0(int, pause)
inline _syscall0(int, sync)
inline _syscall0(pid_t,setsid)
inline _syscall3(int,write,int,fd,const char *,buf,off_t,count)
inline _syscall1(int,dup,int,fd)
inline _syscall3(int,open,const char *,file,int,flag,int,mode)
inline _syscall3(int,execve,const char *,file,char **,argv,char **,envp)
inline _syscall1(int,close,int,fd)
inline _syscall3(pid_t,waitpid,pid_t,pid,int *,wait_stat,int,options)
extern void _exit(int exit_code);

inline pid_t wait(int * wait_stat)
{
	return waitpid(-1,wait_stat,0);
}

char printbuf[1024];

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K        (*(unsigned short *)0x90002)
#define SCREEN_INFO      (*(struct screen_info *)0x90000)
#define DRIVE_INFO       (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV    (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */
#define CMOS_READ(addr)  ({   \
	outb_p(0x80 | addr, 0x70);   \
	inb_p(0x71);                 \
})

#define BCD_TO_BIN(val)  ((val)=((val) & 15) + ((val) >> 4) * 10)

struct drive_info {
	char dummy[32];
} drive_info;
struct screen_info screen_info;

const char *command_line = "loglevel=8 console=ttyS0,115200";
static unsigned long memory_start = 0;
static unsigned long memory_end = 0;
static char term[32];

static char *argv_init[] = { "/bin/init", NULL };
static char *envp_init[] = { "HOME=/", NULL, NULL };

static char *argv_rc[] = { "/bin/sh", NULL };
static char *envp_rc[] = { "HOME=/", NULL, NULL };

static char *argv[] = { "-/bin/sh", NULL };
static char *envp[] = { "HOME=/usr/root", NULL, NULL };

extern void init(void);
extern int vsprintf(char *buf, const char *fmt, va_list args);
extern void init_IRQ(void);
extern long blk_dev_init(long, long);
extern long chr_dev_init(long, long);
extern void sock_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern long kernel_mktime(struct tm *);

#ifdef CONFIG_SCSI
extern void scsi_dev_init(void);
#endif

static int sprintf(char *str, const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    i = vsprintf(str, fmt, args);
    va_end(args);
    return i;
}

/*
 * Initialize system time.
 */
static void time_init(void)
{
    struct tm time;

    do {
        time.tm_sec = CMOS_READ(0);
        time.tm_min = CMOS_READ(2);
        time.tm_hour = CMOS_READ(4);
        time.tm_mday = CMOS_READ(7);
        time.tm_mon = CMOS_READ(8);
        time.tm_year = CMOS_READ(9);
    } while (time.tm_sec != CMOS_READ(0));

    BCD_TO_BIN(time.tm_sec);
    BCD_TO_BIN(time.tm_min);
    BCD_TO_BIN(time.tm_hour);
    BCD_TO_BIN(time.tm_mday);
    BCD_TO_BIN(time.tm_mon);
    BCD_TO_BIN(time.tm_year);
    time.tm_mon--;
    startup_time = kernel_mktime(&time);
}

int start_kernel(void)
{
    /*
     * Interrupts are still disabled. Do necessary setups, then
     * enable them.
     */
#ifdef CONFIG_DEBUG_KERNEL_EARLY
    debug_on_kernel_early();
#endif
    ROOT_DEV = ORIG_ROOT_DEV;
    drive_info = DRIVE_INFO;
    screen_info = SCREEN_INFO;
    sprintf(term, "TERM=con%dx%d", ORIG_VIDEO_COLS, ORIG_VIDEO_LINES);
    envp[1] = term;
    envp_rc[1] = term;
    envp_init[1] = term;
    memory_end = (1<<20) + (EXT_MEM_K<<10);
    memory_end &= 0xfffff000;
    if (memory_end > 16*1024*1024)
        memory_end = 16*1024*1024;
    memory_start = 1024*1024;
    trap_init();
    init_IRQ();
    sched_init();
    memory_start = chr_dev_init(memory_start,memory_end);
    memory_start = blk_dev_init(memory_start,memory_end);
    memory_start = mem_init(memory_start,memory_end);
    buffer_init();
    time_init();
    printk("Linux version " UTS_RELEASE " " __DATE__ " " __TIME__ "\n");
#ifdef CONFIG_HARDDISK
    hd_init();
#endif
#ifdef CONFIG_FLOPPY
    floppy_init();
#endif
    sock_init();
    sti();
#ifdef CONFIG_SCSIS
    scsi_dev_init();
#endif
#ifdef CONFIG_DEBUG_KERNEL_LATER
    debug_on_kernel_later();
#endif
#if defined (CONFIG_DEBUG_USERLAND_EARLY) || \
    defined (CONFIG_DEBUG_USERLAND_SHELL)
    debug_kernel_on_userland_stage();
#endif
    move_to_user_mode();
    if (!fork()) {   /* we count on this going ok */
        init();
    }
/*
 * task[0] is meant to be used as an "idle" task: it may not sleep, but
 * it might do some general things like count free pages or it could be
 * used to implement a reasonable LRU algorithm for the paging routines:
 * anything that can be useful, but shouldn't take time from the real
 * processes.
 *
 * Right now task[0] just does a infinite loop in user mode.
 */
    for (;;)
        /* nothing */;
}

int printf(const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    write(1, printbuf, i = vsprintf(printbuf, fmt, args));
    va_end(args);
    return i;
}

void init(void)
{
    int pid, i;

    setup((void *)&drive_info);
    (void)open("/dev/tty0", O_RDWR, 0);
    (void)dup(0);
    (void)dup(0);
#ifdef CONFIG_DEBUG_USERLAND_SYSCALL
    debug_on_userland_syscall();
#endif
    execve("/etc/init", argv_init, envp_init);
//    execve("/bin/init", argv_init, envp_init);
    /* if this fails, fall through to original stuff */

    if (!(pid = fork())) {
        close(0);
        if (open("/etc/rc", O_RDONLY, 0))
            _exit(1);
        execve("/bin/sh", argv_rc, envp_rc);
        _exit(2);
    }
    if (pid > 0)
        while (pid != wait(&i))
            /* nothing */;
    while (1) {
        if ((pid = fork()) < 0) {
            printf("Fork failed in init\r\n");
            continue;
        }
        if (!pid) {
            close(0);
            close(1);
            close(2);
            setsid();
            (void) open("/dev/tty0", O_RDWR, 0);
            (void) dup(0);
            (void) dup(0);
            _exit(execve("/bin/sh", argv, envp));
        }
        while (1)
            if (pid == wait(&i))
                break;
        printf("\n\rchild %d died with code %04x\n\r", pid, i);
        sync();
    }
    _exit(0);      /* NOTE! _exit, not exit() */
}
