/*
 * TinyShell Final - Full Implementation
 * Phases: 1 (Basic), 2 (Pipes/Redir), 3 (Job Control)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_JOBS 16
#define MAX_COMMANDS 16

// Job States
#define UNDEF 0
#define FG 1    // Foreground
#define BG 2    // Background
#define ST 3    // Stopped

// Job Structure
struct job_t {
    pid_t pid;              // Process ID (Group Leader)
    int jid;                // Job ID [1, 2, ...]
    int state;              // FG, BG, ST
    char cmdline[MAX_LINE]; // The command line
};

struct job_t jobs[MAX_JOBS];
int next_jid = 1;

/* --- Global Shell State --- */
pid_t shell_pgid;
struct termios shell_tmodes;
int shell_terminal;

/* --- Function Prototypes --- */
void eval(char *cmdline);
void process_pipeline(char *cmdline, int bg);
void parse_and_execute_stage(char *cmd_segment, int in_fd, int out_fd, pid_t pgid);
int handle_redirections(char **args, int *in_fd, int *out_fd);

/* --- Job Control Prototypes --- */
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* --- Job Helper Prototypes --- */
void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

int main(int argc, char **argv) {
    char cmdline[MAX_LINE];
    (void)argc; (void)argv;

    // 1. Initialize Shell
    shell_terminal = STDIN_FILENO;
    
    // Ensure we are in foreground
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
        kill(-shell_pgid, SIGTTIN);

    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0) {
        perror("Couldn't put the shell in its own process group");
        exit(1);
    }
    tcsetpgrp(shell_terminal, shell_pgid);
    tcgetattr(shell_terminal, &shell_tmodes);

    initjobs(jobs);

    // 2. Install Signal Handlers
    signal(SIGINT,  sigint_handler);
    signal(SIGTSTP, sigtstp_handler);
    signal(SIGCHLD, sigchld_handler);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    // 3. Main Loop
    while (1) {
        printf("tsh> ");
        fflush(stdout);

        if ((fgets(cmdline, MAX_LINE, stdin) == NULL) && ferror(stdin))
            perror("fgets error");
        
        if (feof(stdin)) { printf("\n"); exit(0); }

        eval(cmdline);
        fflush(stdout);
    }
    exit(0);
}

/**
 * @brief Main evaluation entry point. Checks for & and calls pipeline logic.
 */
void eval(char *cmdline) {
    char *argv[MAX_ARGS];
    char buf[MAX_LINE];
    int bg = 0;
    
    strcpy(buf, cmdline);
    
    // Check for Background (&)
    int argc = 0;
    char *token = strtok(buf, " \t\r\n");
    while (token) {
        argv[argc++] = token;
        token = strtok(NULL, " \t\r\n");
    }
    argv[argc] = NULL;

    if (argc == 0) return;

    // Handle Built-ins immediately
    if (strcmp(argv[0], "exit") == 0) exit(0);
    if (strcmp(argv[0], "jobs") == 0) { listjobs(jobs); return; }
    if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) {
        do_bgfg(argv);
        return;
    }

    // Check for trailing &
    if (strcmp(argv[argc-1], "&") == 0) {
        bg = 1;
    }

    process_pipeline(cmdline, bg);
}

/**
 * @brief Parses pipes and executes the chain. 
 */
void process_pipeline(char *cmdline, int bg) {
    char *commands[MAX_COMMANDS];
    int cmd_count = 0;
    char buf[MAX_LINE];
    strcpy(buf, cmdline);

    // Remove newline
    buf[strcspn(buf, "\n")] = 0;

    // Split by Pipe '|'
    char *token = strtok(buf, "|");
    while (token != NULL && cmd_count < MAX_COMMANDS) {
        commands[cmd_count++] = token;
        token = strtok(NULL, "|");
    }

    // Clean up '&' from the last command if present
    if (bg) {
        char *last = commands[cmd_count-1];
        char *p = strchr(last, '&');
        if (p) *p = ' ';
    }

    int i;
    int prev_pipe_read = STDIN_FILENO;
    int pipefd[2];
    pid_t group_leader_pid = 0; 

    sigset_t mask, prev;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &prev);

    for (i = 0; i < cmd_count; i++) {
        if (i < cmd_count - 1) {
            if (pipe(pipefd) < 0) { perror("pipe"); return; }
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return; }

        if (pid == 0) { // CHILD
            sigprocmask(SIG_SETMASK, &prev, NULL);
            setpgid(0, (i == 0) ? 0 : group_leader_pid);

            if (!bg && i == 0) tcsetpgrp(shell_terminal, getpid());

            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);

            // Input wiring
            if (prev_pipe_read != STDIN_FILENO) {
                dup2(prev_pipe_read, STDIN_FILENO);
                close(prev_pipe_read);
            }
            // Output wiring
            if (i < cmd_count - 1) {
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
                close(pipefd[0]);
            }

            parse_and_execute_stage(commands[i], -1, -1, 0);
            exit(1);
        }
        
        // PARENT
        if (i == 0) group_leader_pid = pid;
        setpgid(pid, group_leader_pid);

        if (prev_pipe_read != STDIN_FILENO) close(prev_pipe_read);
        if (i < cmd_count - 1) {
            close(pipefd[1]);
            prev_pipe_read = pipefd[0];
        }
    }

    addjob(jobs, group_leader_pid, bg ? BG : FG, cmdline);
    sigprocmask(SIG_SETMASK, &prev, NULL);

    if (!bg) {
        waitfg(group_leader_pid);
    } else {
        printf("[%d] (%d) %s\n", pid2jid(group_leader_pid), group_leader_pid, cmdline);
    }
}

/**
 * @brief Parses individual command and executes
 */
void parse_and_execute_stage(char *cmd_segment, int in_fd, int out_fd, pid_t pgid) {
    (void)pgid; // Unused parameter
    char *args[MAX_ARGS];
    int i = 0;
    
    char *token = strtok(cmd_segment, " \t\r\n");
    while (token && i < MAX_ARGS - 1) {
        args[i++] = token;
        token = strtok(NULL, " \t\r\n");
    }
    args[i] = NULL;

    if (args[0] == NULL) exit(0);

    if (handle_redirections(args, &in_fd, &out_fd) < 0) exit(1);

    if (in_fd != -1) { dup2(in_fd, STDIN_FILENO); close(in_fd); }
    if (out_fd != -1) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }

    if (execvp(args[0], args) < 0) {
        printf("%s: Command not found\n", args[0]);
        exit(1);
    }
}

int handle_redirections(char **args, int *in_fd, int *out_fd) {
    int i = 0;
    while (args[i] != NULL) {
        int fd = -1;
        if (strcmp(args[i], "<") == 0) {
            if (args[i+1] == NULL) return -1;
            fd = open(args[i+1], O_RDONLY);
            if (fd < 0) { perror("open <"); return -1; }
            *in_fd = fd;
        } else if (strcmp(args[i], ">") == 0) {
            if (args[i+1] == NULL) return -1;
            fd = open(args[i+1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { perror("open >"); return -1; }
            *out_fd = fd;
        } else if (strcmp(args[i], ">>") == 0) {
            if (args[i+1] == NULL) return -1;
            fd = open(args[i+1], O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) { perror("open >>"); return -1; }
            *out_fd = fd;
        } else {
            i++; continue;
        }
        // Shift args
        int j = i;
        while (args[j+2] != NULL) { args[j] = args[j+2]; j++; }
        args[j] = NULL; args[j+1] = NULL;
    }
    return 0;
}

/* --- Job Control --- */

void waitfg(pid_t pid) {
    tcsetpgrp(shell_terminal, pid);
    
    struct job_t *job = getjobpid(jobs, pid);
    while (job != NULL && job->pid == pid && job->state == FG) {
        sleep(1); 
    }

    tcsetpgrp(shell_terminal, shell_pgid);
}

void do_bgfg(char **argv) {
    struct job_t *job = NULL;
    char *id = argv[1];
    int jid;

    if (id == NULL) { printf("requires PID or %%jobid\n"); return; }

    if (id[0] == '%') {
        jid = atoi(&id[1]);
        job = getjobjid(jobs, jid);
        if (!job) { printf("%%%d: No such job\n", jid); return; }
    } else {
        pid_t pid = atoi(id);
        job = getjobpid(jobs, pid);
        if (!job) { printf("(%d): No such process\n", pid); return; }
    }

    kill(-(job->pid), SIGCONT);

    if (strcmp(argv[0], "bg") == 0) {
        job->state = BG;
        printf("[%d] (%d) %s\n", job->jid, job->pid, job->cmdline);
    } else {
        job->state = FG;
        waitfg(job->pid);
    }
}

void sigchld_handler(int sig) {
    int status;
    pid_t pid;
    (void)sig;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            deletejob(jobs, pid);
        } else if (WIFSTOPPED(status)) {
            struct job_t *job = getjobpid(jobs, pid);
            if (job) {
                job->state = ST;
                printf("Job [%d] (%d) stopped by signal %d\n", job->jid, job->pid, WSTOPSIG(status));
            }
        }
    }
}

void sigint_handler(int sig) { (void)sig; }
void sigtstp_handler(int sig) { (void)sig; }

/* --- Helpers --- */
void clearjob(struct job_t *job) {
    job->pid = 0; job->jid = 0; job->state = UNDEF; job->cmdline[0] = '\0';
}
void initjobs(struct job_t *jobs) {
    for (int i=0; i<MAX_JOBS; i++) clearjob(&jobs[i]);
}
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    if (pid < 1) return 0;
    for (int i=0; i<MAX_JOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = next_jid++;
            strcpy(jobs[i].cmdline, cmdline);
            return 1;
        }
    }
    return 0;
}
int deletejob(struct job_t *jobs, pid_t pid) {
    if (pid < 1) return 0;
    for (int i=0; i<MAX_JOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    for (int i=0; i<MAX_JOBS; i++) if (jobs[i].pid == pid) return &jobs[i];
    return NULL;
}
struct job_t *getjobjid(struct job_t *jobs, int jid) {
    for (int i=0; i<MAX_JOBS; i++) if (jobs[i].jid == jid) return &jobs[i];
    return NULL;
}
int pid2jid(pid_t pid) {
    for (int i=0; i<MAX_JOBS; i++) if (jobs[i].pid == pid) return jobs[i].jid;
    return 0;
}
void listjobs(struct job_t *jobs) {
    for (int i=0; i<MAX_JOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            if (jobs[i].state == BG) printf("Running ");
            else if (jobs[i].state == FG) printf("Foreground ");
            else printf("Stopped ");
            printf("%s\n", jobs[i].cmdline);
        }
    }
}
