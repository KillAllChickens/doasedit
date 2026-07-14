#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct file_info {
    const char *target_path;
    char temp_path[64];
    struct stat st_before;
};

struct file_info *global_files = NULL;
int global_num_files = 0;

void cleanup(void) {
    if (global_files) {
        for (int i = 0; i < global_num_files; i++) {
            if (global_files[i].temp_path[0] != '\0') {
                unlink(global_files[i].temp_path);
            }
        }
    }
}

void handle_sig(int sig) {
    cleanup();
    _exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file1> [file2 ...]\n", argv[0]);
        return 1;
    }

    atexit(cleanup);
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGHUP, handle_sig);

    global_num_files = argc - 1;
    global_files = calloc(global_num_files, sizeof(struct file_info));
    if (!global_files) {
        perror("calloc");
        return 1;
    }

    for (int i = 0; i < global_num_files; i++) {
        global_files[i].target_path = argv[i + 1];
        snprintf(global_files[i].temp_path, sizeof(global_files[i].temp_path),
                 "/tmp/doasedit.XXXXXX");

        int fd = mkstemp(global_files[i].temp_path);
        if (fd < 0) {
            perror("mkstemp");
            exit(1);
        }

        pid_t pid = fork();
        if (pid == 0) {
            dup2(fd, STDOUT_FILENO);
            close(fd);

            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }

            execlp("doas", "doas", "cat", "--", global_files[i].target_path,
                   NULL);
            perror("execlp doas cat");
            exit(1);
        }
        close(fd);

        int status;
        waitpid(pid, &status, 0);

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Failed to read %s (doas/auth failure)\n",
                    global_files[i].target_path);
            exit(1);
        }

        if (stat(global_files[i].temp_path, &global_files[i].st_before) < 0) {
            perror("stat");
            exit(1);
        }
    }

    const char *editor = getenv("DOAS_EDITOR");
    if (!editor)
        editor = getenv("VISUAL");
    if (!editor)
        editor = getenv("EDITOR");
    if (!editor)
        editor = "nano";

    size_t cmd_len = strlen(editor) + 1;
    for (int i = 0; i < global_num_files; i++) {
        cmd_len += strlen(global_files[i].temp_path) + 1;
    }

    char *cmd = malloc(cmd_len);
    if (!cmd) {
        perror("malloc");
        exit(1);
    }

    strcpy(cmd, editor);
    for (int i = 0; i < global_num_files; i++) {
        strcat(cmd, " ");
        strcat(cmd, global_files[i].temp_path);
    }

    int sys_ret = system(cmd);
    free(cmd);

    if (sys_ret == -1) {
        fprintf(stderr, "Failed to start editor.\n");
        exit(1);
    }

    for (int i = 0; i < global_num_files; i++) {
        struct stat st_after;
        if (stat(global_files[i].temp_path, &st_after) == 0) {
            if (global_files[i].st_before.st_mtime != st_after.st_mtime ||
                global_files[i].st_before.st_size != st_after.st_size) {

                pid_t pid = fork();
                if (pid == 0) {
                    int fd = open(global_files[i].temp_path, O_RDONLY);
                    if (fd < 0)
                        exit(1);

                    dup2(fd, STDIN_FILENO);
                    close(fd);

                    int null_fd = open("/dev/null", O_WRONLY);
                    if (null_fd >= 0) {
                        dup2(null_fd, STDOUT_FILENO);
                        close(null_fd);
                    }

                    execlp("doas", "doas", "tee", "--",
                           global_files[i].target_path, NULL);
                    perror("execlp doas tee");
                    exit(1);
                }

                int status;
                waitpid(pid, &status, 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    printf("%s updated\n", global_files[i].target_path);
                } else {
                    fprintf(stderr, "Failed to update %s",
                            global_files[i].target_path);
                }
            } else {
                printf("%s unchanged\n", global_files[i].target_path);
            }
        } else {
            perror("stat after edit");
        }
    }

    return 0;
}
