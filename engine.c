#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define MONITOR_DEVICE "/dev/container_monitor"
#define LOG_DIR "logs"
#define BUFFER_SIZE 16
#define CHUNK 256
#define DEFAULT_SOFT_LIMIT_BYTES (128UL * 1024UL * 1024UL)
#define DEFAULT_HARD_LIMIT_BYTES (256UL * 1024UL * 1024UL)

#if defined(__has_include)
#if __has_include(<sys/ioctl.h>)
#include <sys/ioctl.h>
#else
#include <linux/ioctl.h>
#endif
#if __has_include("monitor_ioctl.h")
#include "monitor_ioctl.h"
#else
#define MONITOR_CONTAINER_ID_LEN CONTAINER_ID_LEN
#define MONITOR_IOC_MAGIC 'm'
struct monitor_request {
    pid_t pid;
    char container_id[MONITOR_CONTAINER_ID_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
};
#define MONITOR_REGISTER _IOW(MONITOR_IOC_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_IOC_MAGIC, 2, struct monitor_request)
#endif
#else
#include <sys/ioctl.h>
#include "monitor_ioctl.h"
#endif

/* ---------------- ENUMS ---------------- */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

/* ---------------- STRUCTS ---------------- */

typedef struct {
    char id[CONTAINER_ID_LEN];
    char data[CHUNK];
} log_item_t;

typedef struct {
    log_item_t items[BUFFER_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} buffer_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    int log_fd;
    void *stack;
    char log_file[PATH_MAX];
    char state[16];
    struct container_record *next;
} container_record_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    int pipe_fd[2];
} child_config_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
} control_request_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    int read_fd;
} producer_args_t;

static buffer_t log_buffer;
static container_record_t *container_list = NULL;
static pthread_mutex_t container_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- BUFFER ---------------- */

static void buffer_init(void)
{
    log_buffer.head = 0;
    log_buffer.tail = 0;
    log_buffer.count = 0;
    pthread_mutex_init(&log_buffer.lock, NULL);
    pthread_cond_init(&log_buffer.not_empty, NULL);
    pthread_cond_init(&log_buffer.not_full, NULL);
}

static void buffer_push(log_item_t item)
{
    pthread_mutex_lock(&log_buffer.lock);
    while (log_buffer.count == BUFFER_SIZE)
        pthread_cond_wait(&log_buffer.not_full, &log_buffer.lock);

    log_buffer.items[log_buffer.tail] = item;
    log_buffer.tail = (log_buffer.tail + 1) % BUFFER_SIZE;
    log_buffer.count++;

    pthread_cond_signal(&log_buffer.not_empty);
    pthread_mutex_unlock(&log_buffer.lock);
}

static log_item_t buffer_pop(void)
{
    log_item_t item;

    pthread_mutex_lock(&log_buffer.lock);
    while (log_buffer.count == 0)
        pthread_cond_wait(&log_buffer.not_empty, &log_buffer.lock);

    item = log_buffer.items[log_buffer.head];
    log_buffer.head = (log_buffer.head + 1) % BUFFER_SIZE;
    log_buffer.count--;

    pthread_cond_signal(&log_buffer.not_full);
    pthread_mutex_unlock(&log_buffer.lock);

    return item;
}

/* ---------------- UTILS ---------------- */

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;

    if (!src)
        src = "";

    snprintf(dst, dst_size, "%s", src);
}

static ssize_t write_all(int fd, const void *buf, size_t count)
{
    const char *cursor = (const char *)buf;
    size_t remaining = count;

    while (remaining > 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += written;
        remaining -= (size_t)written;
    }

    return (ssize_t)count;
}

static ssize_t read_full(int fd, void *buf, size_t count)
{
    char *cursor = (char *)buf;
    size_t total = 0;

    while (total < count) {
        ssize_t nread = read(fd, cursor + total, count - total);
        if (nread == 0)
            break;
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += (size_t)nread;
    }

    return (ssize_t)total;
}

static int send_response(int fd, const char *fmt, ...)
{
    char buffer[4096];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len < 0)
        return -1;
    if ((size_t)len >= sizeof(buffer))
        len = (int)(sizeof(buffer) - 1);

    return (write_all(fd, buffer, (size_t)len) < 0) ? -1 : 0;
}

static container_record_t *find_container_locked(const char *id)
{
    container_record_t *curr = container_list;

    while (curr) {
        if (strncmp(curr->id, id, sizeof(curr->id)) == 0)
            return curr;
        curr = curr->next;
    }

    return NULL;
}

static int ensure_log_dir(void)
{
    if (mkdir(LOG_DIR, 0777) < 0 && errno != EEXIST) {
        perror("mkdir logs");
        return -1;
    }
    return 0;
}

static int monitor_ioctl_request(unsigned int cmd, pid_t pid, const char *id,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    int fd;
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = pid;
    copy_string(req.container_id, sizeof(req.container_id), id);
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;

    fd = open(MONITOR_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (ioctl(fd, cmd, &req) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    close(fd);
    return 0;
}

static int register_with_monitor(pid_t pid, const char *id)
{
    return monitor_ioctl_request(MONITOR_REGISTER, pid, id,
                                 DEFAULT_SOFT_LIMIT_BYTES,
                                 DEFAULT_HARD_LIMIT_BYTES);
}

static int unregister_from_monitor(pid_t pid, const char *id)
{
    return monitor_ioctl_request(MONITOR_UNREGISTER, pid, id, 0, 0);
}

/* ---------------- PRODUCER ---------------- */

static void *producer_thread(void *arg)
{
    producer_args_t *producer = (producer_args_t *)arg;
    char buf[CHUNK];

    for (;;) {
        ssize_t nread = read(producer->read_fd, buf, sizeof(buf) - 1);
        if (nread == 0)
            break;
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        buf[nread] = '\0';

        log_item_t item;
        memset(&item, 0, sizeof(item));
        copy_string(item.id, sizeof(item.id), producer->id);
        copy_string(item.data, sizeof(item.data), buf);
        buffer_push(item);
    }

    close(producer->read_fd);
    free(producer);
    return NULL;
}

/* ---------------- CONSUMER ---------------- */

static void *consumer_thread(void *arg)
{
    (void)arg;

    for (;;) {
        log_item_t item = buffer_pop();
        FILE *file;
        char path[PATH_MAX];

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.id);
        file = fopen(path, "a");
        if (!file)
            continue;

        fputs(item.data, file);
        fclose(file);
    }
}

/* ---------------- CONTAINER ---------------- */

static int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    close(config->pipe_fd[0]);

    if (dup2(config->pipe_fd[1], STDOUT_FILENO) < 0)
        perror("dup2 stdout");
    if (dup2(config->pipe_fd[1], STDERR_FILENO) < 0)
        perror("dup2 stderr");
    close(config->pipe_fd[1]);

    if (sethostname(config->id, strlen(config->id)) < 0)
        perror("sethostname");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount private");

    if (chroot(config->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST)
        perror("mkdir /proc");

    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0)
        perror("mount /proc");

    execl("/memory_hog", "memory_hog", NULL);
    perror("execl");
    return 1;
}

/* ---------------- SUPERVISOR HELPERS ---------------- */

static int start_container(const control_request_t *req, int client_fd)
{
    int pipefd[2] = {-1, -1};
    int producer_fd = -1;
    void *stack = NULL;
    pid_t pid;
    container_record_t *record = NULL;
    producer_args_t *producer = NULL;
    pthread_t producer_tid;
    child_config_t config;

    if (req->container_id[0] == '\0' || req->rootfs[0] == '\0')
        return send_response(client_fd, "ERR invalid start request\n");

    if (access(req->rootfs, F_OK) < 0)
        return send_response(client_fd, "ERR rootfs does not exist: %s\n", req->rootfs);

    pthread_mutex_lock(&container_lock);
    if (find_container_locked(req->container_id)) {
        pthread_mutex_unlock(&container_lock);
        return send_response(client_fd, "ERR container already exists: %s\n", req->container_id);
    }
    pthread_mutex_unlock(&container_lock);

    if (pipe(pipefd) < 0)
        return send_response(client_fd, "ERR pipe failed: %s\n", strerror(errno));

    memset(&config, 0, sizeof(config));
    copy_string(config.id, sizeof(config.id), req->container_id);
    copy_string(config.rootfs, sizeof(config.rootfs), req->rootfs);
    config.pipe_fd[0] = pipefd[0];
    config.pipe_fd[1] = pipefd[1];

    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]);
        close(pipefd[1]);
        return send_response(client_fd, "ERR stack allocation failed\n");
    }

    pid = clone(child_fn, (char *)stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, &config);
    if (pid < 0) {
        int saved_errno = errno;
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        return send_response(client_fd, "ERR clone failed: %s\n", strerror(saved_errno));
    }

    close(pipefd[1]);
    pipefd[1] = -1;

    producer_fd = dup(pipefd[0]);
    if (producer_fd < 0) {
        int saved_errno = errno;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefd[0]);
        free(stack);
        return send_response(client_fd, "ERR dup failed: %s\n", strerror(saved_errno));
    }

    if (register_with_monitor(pid, req->container_id) < 0) {
        int saved_errno = errno;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefd[0]);
        close(producer_fd);
        free(stack);
        return send_response(client_fd, "ERR monitor register failed: %s\n",
                             strerror(saved_errno));
    }

    record = calloc(1, sizeof(*record));
    if (!record) {
        unregister_from_monitor(pid, req->container_id);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefd[0]);
        close(producer_fd);
        free(stack);
        return send_response(client_fd, "ERR container record allocation failed\n");
    }

    producer = calloc(1, sizeof(*producer));
    if (!producer) {
        free(record);
        unregister_from_monitor(pid, req->container_id);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefd[0]);
        close(producer_fd);
        free(stack);
        return send_response(client_fd, "ERR producer allocation failed\n");
    }

    copy_string(record->id, sizeof(record->id), req->container_id);
    record->pid = pid;
    record->log_fd = pipefd[0];
    record->stack = stack;
    snprintf(record->log_file, sizeof(record->log_file), "%s/%s.log", LOG_DIR, req->container_id);
    copy_string(record->state, sizeof(record->state), "running");

    copy_string(producer->id, sizeof(producer->id), req->container_id);
    producer->read_fd = producer_fd;

    pthread_mutex_lock(&container_lock);
    record->next = container_list;
    container_list = record;
    pthread_mutex_unlock(&container_lock);

    if (pthread_create(&producer_tid, NULL, producer_thread, producer) != 0) {
        int saved_errno = errno;
        pthread_mutex_lock(&container_lock);
        if (container_list == record) {
            container_list = record->next;
        } else {
            container_record_t *prev = container_list;
            while (prev && prev->next != record)
                prev = prev->next;
            if (prev)
                prev->next = record->next;
        }
        pthread_mutex_unlock(&container_lock);
        free(producer);
        free(record);
        unregister_from_monitor(pid, req->container_id);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefd[0]);
        close(producer_fd);
        free(stack);
        return send_response(client_fd, "ERR producer thread failed: %s\n",
                             strerror(saved_errno));
    }

    pthread_detach(producer_tid);
    return send_response(client_fd, "OK started id=%s pid=%d\n", req->container_id, pid);
}

static int list_containers(int client_fd)
{
    container_record_t *curr;

    if (send_response(client_fd, "ID\tPID\tSTATE\n") < 0)
        return -1;

    pthread_mutex_lock(&container_lock);
    for (curr = container_list; curr; curr = curr->next) {
        if (send_response(client_fd, "%s\t%d\t%s\n",
                          curr->id, curr->pid, curr->state) < 0) {
            pthread_mutex_unlock(&container_lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&container_lock);

    return 0;
}

static int send_logs(const char *id, int client_fd)
{
    FILE *file;
    char path[PATH_MAX];
    char line[CHUNK];

    if (id[0] == '\0')
        return send_response(client_fd, "ERR missing container id\n");

    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, id);
    file = fopen(path, "r");
    if (!file)
        return send_response(client_fd, "ERR no logs for container %s\n", id);

    while (fgets(line, sizeof(line), file)) {
        if (write_all(client_fd, line, strlen(line)) < 0) {
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    return 0;
}

static int stop_container(const char *id, int client_fd)
{
    container_record_t *record;
    pid_t pid;
    int tries;

    if (id[0] == '\0')
        return send_response(client_fd, "ERR missing container id\n");

    pthread_mutex_lock(&container_lock);
    record = find_container_locked(id);
    if (!record) {
        pthread_mutex_unlock(&container_lock);
        return send_response(client_fd, "ERR unknown container: %s\n", id);
    }

    pid = record->pid;
    if (strncmp(record->state, "running", sizeof(record->state)) != 0 &&
        strncmp(record->state, "stopping", sizeof(record->state)) != 0) {
        char state[sizeof(record->state)];
        copy_string(state, sizeof(state), record->state);
        pthread_mutex_unlock(&container_lock);
        return send_response(client_fd, "OK container %s already %s\n", id, state);
    }

    copy_string(record->state, sizeof(record->state), "stopping");
    pthread_mutex_unlock(&container_lock);

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
        return send_response(client_fd, "ERR SIGTERM failed: %s\n", strerror(errno));

    for (tries = 0; tries < 20; tries++) {
        pthread_mutex_lock(&container_lock);
        record = find_container_locked(id);
        if (!record) {
            pthread_mutex_unlock(&container_lock);
            break;
        }
        if (strncmp(record->state, "running", sizeof(record->state)) != 0 &&
            strncmp(record->state, "stopping", sizeof(record->state)) != 0) {
            pthread_mutex_unlock(&container_lock);
            unregister_from_monitor(pid, id);
            return send_response(client_fd, "OK stopped id=%s pid=%d\n", id, pid);
        }
        pthread_mutex_unlock(&container_lock);
        usleep(100000);
    }

    if (kill(pid, SIGKILL) < 0 && errno != ESRCH)
        return send_response(client_fd, "ERR SIGKILL failed: %s\n", strerror(errno));

    unregister_from_monitor(pid, id);
    return send_response(client_fd, "OK kill requested id=%s pid=%d\n", id, pid);
}

static void *reaper_thread(void *arg)
{
    (void)arg;

    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        container_record_t *curr;
        char container_id[CONTAINER_ID_LEN];
        bool found = false;

        if (pid < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ECHILD) {
                sleep(1);
                continue;
            }
            continue;
        }

        pthread_mutex_lock(&container_lock);
        memset(container_id, 0, sizeof(container_id));
        for (curr = container_list; curr; curr = curr->next) {
            if (curr->pid == pid) {
                copy_string(container_id, sizeof(container_id), curr->id);
                found = true;

                if (WIFEXITED(status))
                    copy_string(curr->state, sizeof(curr->state), "exited");
                else if (WIFSIGNALED(status))
                    copy_string(curr->state, sizeof(curr->state), "stopped");
                else
                    copy_string(curr->state, sizeof(curr->state), "unknown");

                if (curr->stack) {
                    free(curr->stack);
                    curr->stack = NULL;
                }
                if (curr->log_fd >= 0) {
                    close(curr->log_fd);
                    curr->log_fd = -1;
                }
                break;
            }
        }
        pthread_mutex_unlock(&container_lock);

        if (found)
            unregister_from_monitor(pid, container_id);
    }
}

static void *client_handler_thread(void *arg)
{
    int client_fd = *(int *)arg;
    control_request_t req;

    free(arg);
    memset(&req, 0, sizeof(req));

    if (read_full(client_fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        send_response(client_fd, "ERR failed to read request\n");
        close(client_fd);
        return NULL;
    }

    switch (req.kind) {
    case CMD_START:
        start_container(&req, client_fd);
        break;
    case CMD_PS:
        list_containers(client_fd);
        break;
    case CMD_LOGS:
        send_logs(req.container_id, client_fd);
        break;
    case CMD_STOP:
        stop_container(req.container_id, client_fd);
        break;
    default:
        send_response(client_fd, "ERR unsupported command\n");
        break;
    }

    close(client_fd);
    return NULL;
}

/* ---------------- CLIENT ---------------- */

static int send_control_request(control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    char buf[2048];
    ssize_t nread;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    copy_string(addr.sun_path, sizeof(addr.sun_path), CONTROL_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (write_all(fd, req, sizeof(*req)) < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    while ((nread = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[nread] = '\0';
        fputs(buf, stdout);
    }

    if (nread < 0) {
        perror("read");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

/* ---------------- SUPERVISOR ---------------- */

static int run_supervisor(void)
{
    int server_fd;
    pthread_t consumer_tid;
    pthread_t reaper_tid;
    struct sockaddr_un addr;

    if (ensure_log_dir() < 0)
        return 1;

    buffer_init();

    if (pthread_create(&consumer_tid, NULL, consumer_thread, NULL) != 0) {
        perror("pthread_create consumer");
        return 1;
    }
    pthread_detach(consumer_tid);

    if (pthread_create(&reaper_tid, NULL, reaper_thread, NULL) != 0) {
        perror("pthread_create reaper");
        return 1;
    }
    pthread_detach(reaper_tid);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    copy_string(addr.sun_path, sizeof(addr.sun_path), CONTROL_PATH);

    unlink(CONTROL_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        unlink(CONTROL_PATH);
        return 1;
    }

    printf("Supervisor listening on %s\n", CONTROL_PATH);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        pthread_t client_tid;
        int *client_arg;

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        client_arg = malloc(sizeof(*client_arg));
        if (!client_arg) {
            send_response(client_fd, "ERR out of memory\n");
            close(client_fd);
            continue;
        }

        *client_arg = client_fd;
        if (pthread_create(&client_tid, NULL, client_handler_thread, client_arg) != 0) {
            perror("pthread_create client");
            free(client_arg);
            close(client_fd);
            continue;
        }

        pthread_detach(client_tid);
    }

    return 0;
}

/* ---------------- MAIN ---------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor\n"
            "  %s start <id> <rootfs>\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    memset(&req, 0, sizeof(req));

    if (strcmp(argv[1], "start") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 1;
        }
        req.kind = CMD_START;
        copy_string(req.container_id, sizeof(req.container_id), argv[2]);
        copy_string(req.rootfs, sizeof(req.rootfs), argv[3]);
    } else if (strcmp(argv[1], "ps") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 1;
        }
        req.kind = CMD_PS;
    } else if (strcmp(argv[1], "logs") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }
        req.kind = CMD_LOGS;
        copy_string(req.container_id, sizeof(req.container_id), argv[2]);
    } else if (strcmp(argv[1], "stop") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }
        req.kind = CMD_STOP;
        copy_string(req.container_id, sizeof(req.container_id), argv[2]);
    } else {
        usage(argv[0]);
        return 1;
    }

    return send_control_request(&req);
}