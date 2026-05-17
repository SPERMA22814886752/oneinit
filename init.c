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
#include <sys/select.h>

volatile sig_atomic_t current_runlevel = 3; 
volatile sig_atomic_t child_died = 0;
volatile sig_atomic_t shutdown_requested = 0;

/* массив пидов для консолей, шоб было куда падать если что */
pid_t shell_pids[2] = {0, 0};
const char *tty_names[2] = {"tty1", "tty2"};

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
static pid_t spawn_shell(const char *tty);
static void emergency_shell(void);
static void graceful_shutdown(void);
static int run_busybox(char *const argv[]);
static void setup_ipc(void);
static void update_status_file(void);

/* функции для утилит */
static int telinit_main(int argc, char *argv[]);
static int instatus_main(int argc, char *argv[]);

static void install_own_symlinks(void) {
    char exe_path[256];
    
    do_log("installing wilix init symlinks...\n");

    /* узнаем реальный путь к нашему бинарнику */
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
    } else {
        /* фоллбэк, если вдруг /proc отвалился */
        strcpy(exe_path, "/sbin/init");
    }

    /* создаем telinit */
    if (symlink(exe_path, "/usr/bin/telinit") != 0 && errno != EEXIST) {
        do_log("warning: cannot create /sbin/telinit: %s\n", strerror(errno));
    }

    /* создаем in-status */
    if (symlink(exe_path, "/usr/bin/in-status") != 0 && errno != EEXIST) {
        do_log("warning: cannot create /usr/bin/in-status: %s\n", strerror(errno));
    }
}

int main(int argc, char *argv[]) {
    /* проверяем, как нас вызвали (симлинки) */
    char *progname = strrchr(argv[0], '/');
    progname = progname ? progname + 1 : argv[0];

    if (strcmp(progname, "telinit") == 0) return telinit_main(argc, argv);
    if (strcmp(progname, "in-status") == 0) return instatus_main(argc, argv);

    /* если мы тут, значит мы - батька PID 1 */
    if (getpid() != 1) {
        fprintf(stderr, "error: init must be run as PID 1\n");
        return 1;
    }

    setup_signals();

    /* базовые ФС */
    do_mount("proc", "/proc", "proc", 0);
    do_mount("sysfs", "/sys", "sysfs", 0);
    do_mount("none", "/dev", "devtmpfs", 0);
    
    /* фикс: tmpfs для /run, шоб сокеты и FIFO жили счастливо */
    mkdir("/run", 0755);
    do_mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC);

    mknod("/dev/fb0", S_IFCHR | 0666, makedev(29, 0));   /* framebuffer */
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
    
    /* фикс: создаем папку перед монтированием, шоб не ругалось */
    mkdir("/dev/pts", 0755);
    do_mount("devpts", "/dev/pts", "devpts", 0);

    /* девайсы */
    mknod("/dev/tty1", S_IFCHR | 0666, makedev(4, 1));
    mknod("/dev/tty2", S_IFCHR | 0666, makedev(4, 2)); /* добавил tty2 для вкуса */

    mkdir("/var", 0755);
    mkdir("/var/log", 0755);

    do_log("================================\n");
    do_log("init started. oneinit v0.7\n");

    parse_cmdline();
    setup_ipc(); /* создаем канал связи с telinit */

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

    install_busybox_symlinks();
    load_kernel_modules();

    install_own_symlinks();

    clear_screen();
    print_banner();

    /* скрипты (теперь y/n работают норм) */
    run_init_scripts();

    /* секьюрность, хуле. проверяем есть ли ваще файл, шоб не спамить ошибками */
    if (access("/usr/bin/doas", F_OK) == 0) {
        do_log("setting doas permissions...\n");
        system("chown root:root /usr/bin/doas");
        system("chmod 4755 /usr/bin/doas");
    }
    if (access("/etc/doas.conf", F_OK) == 0) {
        system("chown root:root /etc/doas.conf");
        system("chmod 600 /etc/doas.conf");
    }

    update_status_file();
    do_log("entering main init loop. runlevel = %d\n", current_runlevel);

    int fifo_fd = open("/run/initctl", O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0) do_log("warning: cannot open /run/initctl\n");

    /* главный цикл (теперь с select, шоб не грузить проц) */
    while (!shutdown_requested) {
        /* спавним гетти на терминалах, если они сдохли */
        if (current_runlevel == 3 || current_runlevel == 5) {
            for (int i = 0; i < 2; i++) {
                if (shell_pids[i] == 0) {
                    shell_pids[i] = spawn_shell(tty_names[i]);
                    if (shell_pids[i] < 0) {
                        do_log("cannot spawn getty on %s!\n", tty_names[i]);
                    }
                }
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        if (fifo_fd >= 0) FD_SET(fifo_fd, &rfds);

        struct timeval tv;
        tv.tv_sec = 2; /* просыпаемся раз в 2 секунды шоб чекать зомбаков наверняка */
        tv.tv_usec = 0;

        int retval = select((fifo_fd >= 0 ? fifo_fd + 1 : 0), &rfds, NULL, NULL, &tv);

        /* читаем команды от telinit */
        if (retval > 0 && fifo_fd >= 0 && FD_ISSET(fifo_fd, &rfds)) {
            char cmd;
            if (read(fifo_fd, &cmd, 1) == 1) {
                if (cmd >= '0' && cmd <= '6') {
                    int new_rl = cmd - '0';
                    do_log("received command to switch runlevel from %d to %d\n", current_runlevel, new_rl);
                    current_runlevel = new_rl;
                    update_status_file();

                    if (current_runlevel == 0 || current_runlevel == 6) {
                        shutdown_requested = 1;
                    } else if (current_runlevel == 1) {
                        /* рубим гетти и идем в single user */
                        kill(-1, SIGTERM);
                        emergency_shell();
                        shutdown_requested = 1; /* после emergency сразу ребут/халт */
                    }
                }
            }
        }

        /* убираем трупы */
        if (child_died) {
            child_died = 0;
            int status;
            pid_t p;
            while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
                for (int i = 0; i < 2; i++) {
                    if (p == shell_pids[i]) {
                        do_log("getty on %s died (status %d), will respawn...\n", tty_names[i], status);
                        shell_pids[i] = 0; 
                    }
                }
            }
        }
    }

    if (fifo_fd >= 0) close(fifo_fd);
    graceful_shutdown();
    return 0;
}

/* ================== УТИЛИТЫ УПРАВЛЕНИЯ ================== */

/* telinit: шлет циферку в FIFO для PID 1 */
static int telinit_main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: telinit [0-6]\n");
        return 1;
    }
    
    char cmd = argv[1][0];
    if (cmd < '0' || cmd > '6') {
        fprintf(stderr, "invalid runlevel: %c\n", cmd);
        return 1;
    }

    int fd = open("/run/initctl", O_WRONLY);
    if (fd < 0) {
        perror("cannot connect to init (/run/initctl)");
        return 1;
    }

    write(fd, &cmd, 1);
    close(fd);
    printf("telinit: signal %c sent to init.\n", cmd);
    return 0;
}

/* in-status: читает файлик состояния, который заботливо пишет init */
static int instatus_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("=== wilix v0.7 init status ===\n");
    
    FILE *f = fopen("/run/init_status", "r");
    if (!f) {
        printf("State: UNKNOWN (is init running?)\n");
        return 1;
    }
    
    char line[256];
    while(fgets(line, sizeof(line), f)) {
        printf("%s", line);
    }
    fclose(f);

    /* выводим uptime до кучи */
    f = fopen("/proc/uptime", "r");
    if (f) {
        double up;
        if (fscanf(f, "%lf", &up) == 1) {
            printf("Uptime: %.2f seconds\n", up);
        }
        fclose(f);
    }
    printf("==============================\n");
    return 0;
}

/* ================== ВНУТРЯНКА INIT ================== */

static void setup_ipc(void) {
    unlink("/run/initctl");
    if (mkfifo("/run/initctl", 0600) != 0) {
        do_log("failed to create /run/initctl: %s\n", strerror(errno));
    }
}

static void update_status_file(void) {
    FILE *f = fopen("/run/init_status", "w");
    if (f) {
        fprintf(f, "PID 1: running\n");
        fprintf(f, "Current Runlevel: %d\n", current_runlevel);
        fclose(f);
    }
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
    else if (sig == SIGTERM || sig == SIGUSR1) { shutdown_requested = 1; current_runlevel = 0; }
    else if (sig == SIGINT) { shutdown_requested = 1; current_runlevel = 6; }
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
    while(1) pause(); /* ждем смерти */
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
    char hname[256] = "wilix";
    FILE *f = fopen("/etc/hostname", "r");
    if (f) {
        if (fgets(hname, sizeof(hname), f)) {
            hname[strcspn(hname, "\r\n")] = 0; /* режем перенос строки */
        }
        fclose(f);
    }
    if (hname[0] == '\0') strcpy(hname, "wilix");

    if (sethostname(hname, strlen(hname)) != 0) {
        do_log("sethostname failed: %s\n", strerror(errno));
    }
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
    printf("  wilix v0.7 \n");
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

static pid_t spawn_shell(const char *tty) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    
    if (pid == 0) {
        if (setsid() == -1) {
            perror("setsid");
            _exit(1);
        }

        usleep(100000);
        char devpath[32];
        snprintf(devpath, sizeof(devpath), "/dev/%s", tty);

        /* основной план: busybox getty */
        execl("/bin/busybox", "busybox", "getty", "38400", tty, (char *)NULL);
        
        /* если busybox getty обломался, фолбэк: */
        int fd = open(devpath, O_RDWR);
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
