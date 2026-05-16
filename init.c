#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <sys/sysmacros.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>

volatile sig_atomic_t current_runlevel = 3; 
volatile sig_atomic_t child_died = 0;
pid_t shell_pid = 0;

static void setup_signals(void);
static void do_log(const char *fmt, ...);
static int do_mount(const char *source, const char *target, const char *fstype, unsigned long flags);
static void parse_cmdline(void);
static void setup_console_and_tty(void);
static void set_env(void);
static void set_hostname(void);
static void install_busybox_symlinks(void);
static void load_kernel_modules(void);
static void clear_screen(void);
static void print_banner(void);
static void run_init_scripts(void);
static pid_t spawn_shell(void);
static void emergency_shell(void);
static void graceful_shutdown(void);
static int run_busybox(char *const argv[]);

int main(void) {
    setup_signals();

    /* базовые ФС */
    do_mount("proc", "/proc", "proc", 0);
    do_mount("sysfs", "/sys", "sysfs", 0);
    do_mount("none", "/dev", "devtmpfs", 0);
    mknod("/dev/fb0", S_IFCHR | 0666, makedev(29, 0));   /* framebuffer */
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
    
    /* фикс: создаем папку перед монтированием, шоб не ругалось */
    mkdir("/dev/pts", 0755);
    do_mount("devpts", "/dev/pts", "devpts", 0);

    /* девайсы */
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
    mknod("/dev/tty1", S_IFCHR | 0666, makedev(4, 1));

    mkdir("/var", 0755);
    mkdir("/var/log", 0755);

    do_log("================================\n");
    do_log("init started. oneinit v0.6\n");

    parse_cmdline();

    if (current_runlevel == 0 || current_runlevel == 6) {
        graceful_shutdown();
    }
    if (current_runlevel == 1) {
        do_log("runlevel 1 requested. dropping to emergency shell.\n");
        emergency_shell();
        current_runlevel = 0;
        graceful_shutdown();
    }

    setup_console_and_tty();
    set_env();
    set_hostname();

    /* раз мы оставили busybox, пусть он сам сделает симлинки (типа /bin/sh) */
    install_busybox_symlinks();
    load_kernel_modules();

    clear_screen();
    print_banner();

    /* скрипты (теперь y/n работают норм) */
    run_init_scripts();

    do_log("setting doas permissions...\n");
    system("chown root:root /usr/bin/doas");
    system("chmod 4755 /usr/bin/doas");
    system("chown root:root /etc/doas.conf");
    system("chmod 600 /etc/doas.conf");

    do_log("entering main init loop. runlevel = %d\n", current_runlevel);

    /* цикл респавна */
    while (current_runlevel == 3 || current_runlevel == 5) {
        if (shell_pid == 0) {
            shell_pid = spawn_shell();
            if (shell_pid < 0) {
                do_log("cannot spawn getty! dropping to emergency.\n");
                emergency_shell();
            }
        }

        if (child_died) {
            child_died = 0;
            int status;
            pid_t p;
            while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
                if (p == shell_pid) {
                    do_log("getty/login died (status %d), respawning...\n", status);
                    shell_pid = 0; 
                }
            }
        }
        usleep(500000); 
    }

    graceful_shutdown();
    return 0;
}

static void do_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    FILE *f = fopen("/var/log/init.log", "a");
    if (f) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        fprintf(f, "[%02d:%02d:%02d] init: ", tm->tm_hour, tm->tm_min, tm->tm_sec);
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fclose(f);
    }
}

static void sig_handler(int sig) {
    if (sig == SIGCHLD) child_died = 1;
    else if (sig == SIGTERM || sig == SIGUSR1) current_runlevel = 0; 
    else if (sig == SIGINT) current_runlevel = 6; 
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags = SA_RESTART; 

    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL); 
    sigaction(SIGUSR1, &sa, NULL); 

    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
}

static void parse_cmdline(void) {
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return;
    char line[1024];
    if (fgets(line, sizeof(line), f)) {
        if (strstr(line, "runlevel=0") || strstr(line, " 0")) current_runlevel = 0;
        else if (strstr(line, "runlevel=1") || strstr(line, " single") || strstr(line, " 1")) current_runlevel = 1;
        else if (strstr(line, "runlevel=3") || strstr(line, " 3")) current_runlevel = 3;
        else if (strstr(line, "runlevel=5") || strstr(line, " 5")) current_runlevel = 5; 
        else if (strstr(line, "runlevel=6") || strstr(line, " 6")) current_runlevel = 6;
    }
    fclose(f);
}

static void graceful_shutdown(void) {
    do_log("\n*** INITIATING SHUTDOWN SEQUENCE ***\n");
    do_log("sending SIGTERM to all processes...\n");
    kill(-1, SIGTERM);
    sleep(2); 

    do_log("sending SIGKILL to stubborn bastards...\n");
    kill(-1, SIGKILL);
    
    do_log("syncing disks...\n");
    sync();

    do_log("unmounting filesystems...\n");
    system("/bin/busybox umount -a -r"); 

    if (current_runlevel == 6) {
        do_log("rebooting system. poehali!\n");
        reboot(RB_AUTOBOOT);
    } else {
        do_log("powering off. spoki noki.\n");
        reboot(RB_POWER_OFF);
    }
    while(1) pause();
}

static void emergency_shell(void) {
    do_log("DROPPING TO EMERGENCY SHELL!\n");
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/tty1", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }
        setenv("PS1", "(emergency) # ", 1);
        execl("/bin/busybox", "busybox", "sh", (char *)NULL);
        execl("/bin/bash", "bash", (char *)NULL);
        _exit(1); 
    }
    waitpid(pid, NULL, 0);
}

static int do_mount(const char *source, const char *target, const char *fstype, unsigned long flags) {
    if (mount(source, target, fstype, flags, NULL) != 0) {
        do_log("mount fail: %s on %s (%s): %s\n", source, target, fstype, strerror(errno));
        return -1;
    }
    return 0;
}

static void setup_console_and_tty(void) {
    int fd = open("/dev/tty1", O_RDWR);
    if (fd < 0) {
        do_log("cannot open /dev/tty1: %s\n", strerror(errno));
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO);
        execl("/bin/busybox", "busybox", "stty", "sane", (char *)NULL);
        _exit(1);
    }
    waitpid(pid, NULL, 0);

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    ioctl(fd, TIOCSCTTY, 1);
    if (fd > 2) close(fd);
}

static void set_env(void) {
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/", 1);
    setenv("SSL_CERT_FILE", "/etc/ssl/certs/ca-certificates.crt", 1);
}

static void set_hostname(void) {
    if (sethostname("wilix", 5) != 0) do_log("sethostname failed: %s\n", strerror(errno));
}

static void install_busybox_symlinks(void) {
    do_log("installing busybox symlinks...\n");
    char *const args[] = { "/bin/busybox", "busybox", "--install", "-s", NULL };
    run_busybox(args);
}

static void load_kernel_modules(void) {
    DIR *dir;
    struct dirent *ent;
    char modname[256];
    struct stat st;

    if (stat("/lib/modules", &st) != 0 || !S_ISDIR(st.st_mode)) return;

    dir = opendir("/lib/modules");
    if (!dir) return;

    do_log("loading kernel modules...\n");
    while ((ent = readdir(dir)) != NULL) {
        char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".ko") != 0) continue;

        strncpy(modname, ent->d_name, dot - ent->d_name);
        modname[dot - ent->d_name] = '\0';

        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/busybox", "busybox", "modprobe", modname, (char *)NULL);
            _exit(1);
        }
        waitpid(pid, NULL, 0);
    }
    closedir(dir);
}

static void clear_screen(void) {
    char *const args[] = { "/bin/busybox", "busybox", "clear", NULL };
    run_busybox(args);
}

static void print_banner(void) {
    struct utsname uts;
    printf("==========================================\n");
    printf("  wilix v0.5 \n");
    printf("==========================================\n\n");
    printf("welcome to linux!\n\n");
    if (uname(&uts) == 0) {
        printf("%s %s %s %s %s\n", uts.sysname, uts.nodename, uts.release, uts.version, uts.machine);
    }
    printf("\n");
    
    char *const args[] = { "/bin/bash", "--version", NULL };
    run_busybox(args);
    printf("==========================================\n");
}

static void run_init_scripts(void) {
    struct dirent **namelist;
    int n;

    if (access("/etc/init.d", F_OK) != 0) return;

    n = scandir("/etc/init.d", &namelist, NULL, alphasort);
    if (n < 0) {
        do_log("cannot scan /etc/init.d\n");
        return;
    }

    for (int i = 0; i < n; i++) {
        if (namelist[i]->d_name[0] == 'S') {
            char path[512];
            snprintf(path, sizeof(path), "/etc/init.d/%s", namelist[i]->d_name);
            if (access(path, X_OK) != 0) continue;

            do_log("starting: %s ... \n", namelist[i]->d_name);
            pid_t pid = fork();
            if (pid == 0) {
                execl(path, namelist[i]->d_name, "start", (char *)NULL);
                _exit(1);
            }
            waitpid(pid, NULL, 0);
            do_log("[%s OK]\n", namelist[i]->d_name);
        }
    }

    for (int i = 0; i < n; i++) {
        if (namelist[i]->d_name[0] != 'S' && namelist[i]->d_name[0] != '.') {
            char path[512];
            snprintf(path, sizeof(path), "/etc/init.d/%s", namelist[i]->d_name);
            if (access(path, X_OK) != 0) continue;

            do_log("running: %s ... \n", namelist[i]->d_name);
            pid_t pid = fork();
            if (pid == 0) {
                execl(path, namelist[i]->d_name, (char *)NULL);
                _exit(1);
            }
            waitpid(pid, NULL, 0);
            do_log("[%s OK]\n", namelist[i]->d_name);
        }
    }

    for (int i = 0; i < n; i++) free(namelist[i]);
    free(namelist);
}

static pid_t spawn_shell(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    
    if (pid == 0) {
        if (setsid() == -1) {
            perror("setsid");
            _exit(1);
        }

        usleep(100000);

        /* основной план: busybox getty на tty1 */
        execl("/bin/busybox", "busybox", "getty", "38400", "tty1", (char *)NULL);
        
        /* если busybox getty обломался, фолбэк: */
        int fd = open("/dev/tty1", O_RDWR);
        if (fd >= 0) {
            ioctl(fd, TIOCSCTTY, 1);
            dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
            
            /* пробуем дернуть login напрямую */
            execl("/bin/busybox", "busybox", "login", "-p", (char *)NULL);
            /* если и логина нет, кидаем голый bash шоб ты мог спасти систему */
            execl("/bin/bash", "bash", (char *)NULL);
            /* на самый крайняк - sh из busybox */
            execl("/bin/busybox", "busybox", "sh", (char *)NULL);
        }
        
        /* ну тут уже полномочия всё */
        fprintf(stderr, "fatal: busybox is missing or broken. system is fucked.\n");
        sleep(3);
        _exit(1);
    }
    
    return pid;
}

static int run_busybox(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
