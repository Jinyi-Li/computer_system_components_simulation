/*
 * tsh - A tiny shell program with job control
 *
 * It supports four builtin command: fg, bg, jobs, and quit. 
 * For non-builtin commands, it runs the corresponding executable if exists.
 * It supports I/O redirection, i.e. reading from/writing to files.
 *
 * It specifically handles three signals: SIGCHLD, SIGINT, and SIGTSTP.
 *
 * Author: Jinyi Li
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "tsh_helper.h"
#include "csapp.h"

/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

/* A global atomic flag for the parent to get notified by SIGCHLD handler. */
volatile sig_atomic_t waitpidNeeded;

/* Function prototypes */
pid_t get_valid_pid(struct cmdline_tokens token);

int builtin_command(struct cmdline_tokens token, const char *cmdline);

void eval(const char *cmdline);

void sigchld_handler(int sig);

void sigtstp_handler(int sig);

void sigint_handler(int sig);

void sigquit_handler(int sig);

void cleanup(void);

/*
 * main - The main routine of the program.
 *
 * It's the starting point of the program.
 *
 * @param argc number of arguments
 * @param argv array of arguments
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE_TSH]; // Cmdline for fgets
    bool emit_prompt = true;   // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    Dup2(STDOUT_FILENO, STDERR_FILENO);

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
            case 'h': // Prints help message
                usage();
                break;
            case 'v': // Emits additional diagnostic info
                verbose = true;
                break;
            case 'p': // Disables prompt printing
                emit_prompt = false;
                break;
            default:
                usage();
        }
    }

    // Create environment variable
    if (putenv("MY_ENV=42") < 0) {
        perror("putenv");
        exit(1);
    }

    // Set buffering mode of stdout to line buffering.
    // This prevents lines from being printed in the wrong order.
    if (setvbuf(stdout, NULL, _IOLBF, 0) < 0) {
        perror("setvbuf");
        exit(1);
    }

    // Initialize the job list
    init_job_list();

    // Register a function to clean up the job list on program termination.
    // The function may not run in the case of abnormal termination (e.g. when
    // using _exit or terminating due to a signal handler), so in those cases,
    // we trust that the OS will clean up any remaining resources.
    if (atexit(cleanup) < 0) {
        perror("atexit");
        exit(1);
    }

    // Install the signal handlers
    Signal(SIGINT, sigint_handler);   // Handles Ctrl-C
    Signal(SIGTSTP, sigtstp_handler); // Handles Ctrl-Z
    Signal(SIGCHLD, sigchld_handler); // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler);

    // Execute the shell's read/eval loop
    while (true) {
        if (emit_prompt) {
            printf("%s", prompt);

            // We must flush stdout since we are not printing a full line.
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin)) {
            app_error("fgets error");
        }

        if (feof(stdin)) {
            // End of file (Ctrl-D)
            printf("\n");
            return 0;
        }

        // Remove any trailing newline
        char *newline = strchr(cmdline, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        // Evaluate the command line
        eval(cmdline);
    }

    return -1; // control never reaches here
}

/*
 * Validate command line argument as a job id or a process id.
 * Print info messages if there's no such job or process with the given id.
 *
 * @return the corresponding process id if valid, 0 otherwise.
 */
pid_t get_valid_pid(struct cmdline_tokens token) {
    jid_t jobId;
    pid_t processId;

    char *id = token.argv[token.argc - 1];
    // if pass a job id
    if (id[0] == '%') {
        int jid = atoi(id + 1); // escape % sign
        if (jid != 0) {
            jobId = jid;
            if (job_exists(jobId)) {
                processId = job_get_pid(jobId);
                if (processId != 0) {
                    return processId;
                }
            }
        }
        // if invalid job id
        Sio_printf("%s: No such job\n", id);
        return 0;
    } else {
        // if pass a process id
        processId = atoi(id);
        jobId = job_from_pid(processId);
        if (jobId != 0) {
            return processId;
        } else {
            // if invalid pid
            Sio_printf("(%s): No such process\n", id);
            return 0;
        }
    }
}

/*
 * Handle builtin command specified in the given command line token.
 * It supports I/O redirection for the `job` command.
 * Print info messages if redirected outfile cannot be opened.
 *
 * @return 1 if builtin command is executed; 0 if non-builtin command.
 */
int builtin_command(struct cmdline_tokens token, const char *cmdline) {
    builtin_state state;
    sigset_t mask, prev;
    Sigemptyset(&mask);
    Sigaddset(&mask, SIGINT);
    Sigaddset(&mask, SIGTSTP);
    Sigaddset(&mask, SIGCHLD);
    int outputFileId;
    jid_t jobId;
    pid_t processId;

    state = token.builtin;

    // execute the builtin 'quit' command
    if (state == BUILTIN_QUIT) {
        exit(0);
    } else if (state == BUILTIN_NONE) {
        // if state == BUILTIN_NONE or others.
        return 0;
    } else {
        Sigprocmask(SIG_BLOCK, &mask, &prev);
        // execute the builtin 'jobs' command
        if (state == BUILTIN_JOBS) {
            if (token.outfile == NULL) {
                list_jobs(STDOUT_FILENO);
            } else {
                outputFileId = Open(token.outfile, O_CREAT | O_TRUNC
                | O_WRONLY, DEF_MODE);
                if (outputFileId == 0) {
                    Sio_error("Cannot open file.");
                } else {
                    list_jobs(outputFileId);
                }
                Close(outputFileId);
            }
        }
        // execute the builtin 'bg' command
        if (state == BUILTIN_BG) {
            processId = get_valid_pid(token);
            if (processId) {
                jobId = job_from_pid(processId);
                Kill(-processId, SIGCONT);
                job_set_state(jobId, BG);
                Sio_printf("[%d] (%d) %s\n", jobId, processId,
                           job_get_cmdline(jobId));
            }
        }
        // execute the builtin 'fg' command
        if (state == BUILTIN_FG) {
            processId = get_valid_pid(token);
            if (processId) {
                jobId = job_from_pid(processId);
                waitpidNeeded = 0;
                Kill(-processId, SIGCONT);
                job_set_state(jobId, FG);
                while (!waitpidNeeded) {
                    Sigsuspend(&prev);
                }
            }
        }
        Sigprocmask(SIG_SETMASK, &prev, NULL);
        return 1;
    }
}

/*
 * Main routine that parses, interprets, and executes the command line.
 * It runs builtin jobs immediately in the main routine,
 * and it forks a child to run a non-builtin command.
 *
 * The parent process accepts signals from children and handles it.
 *
 * @param cmdline the command line pointer.
 */
void eval(const char *cmdline) {
    parseline_return parse_result;
    struct cmdline_tokens token;
    pid_t processId;
    jid_t jobId;
    sigset_t mask, prev;
    job_state state;
    int isBuiltinCmd, outputFileId = 0, inputFileId = 0;

    // create signal sets
    Sigemptyset(&mask);
    Sigaddset(&mask, SIGINT);
    Sigaddset(&mask, SIGTSTP);
    Sigaddset(&mask, SIGCHLD);

    // Parse command line
    parse_result = parseline(cmdline, &token);
    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY) {
        return;
    }

    // if builtin command, execute it; else, fork a child and run it in there.
    isBuiltinCmd = builtin_command(token, cmdline);

    if (!isBuiltinCmd) {
        Sigprocmask(SIG_BLOCK, &mask, &prev);

        // in child process
        if ((processId = Fork()) == 0) {
            Setpgid(0, 0);
            // unblock SIGCHLD
            Sigprocmask(SIG_SETMASK, &prev, NULL);

            // handle I/O redirection if needed
            if (token.infile != NULL) {
                inputFileId = Open(token.infile, O_RDONLY, DEF_MODE);
                if (inputFileId == 0) {
                    Sio_error("Cannot open file.");
                } else {
                    Dup2(inputFileId, STDIN_FILENO);
                }
            }
            if (token.outfile != NULL) {
                outputFileId = Open(token.outfile, O_CREAT | O_TRUNC | O_WRONLY,
                                    DEF_MODE);
                if (outputFileId == 0) {
                    Sio_error("Cannot open file.");
                } else {
                    Dup2(outputFileId, STDOUT_FILENO);
                }
            }

            Execve(token.argv[0], token.argv, environ);

            // close I/O files
            if (outputFileId != 0) {
                Close(outputFileId);
            }
            if (inputFileId != 0) {
                Close(inputFileId);
            }
        }

        // in parent process
        if (parse_result == PARSELINE_FG) {
            waitpidNeeded = 0;
            state = FG;
        } else {
            state = BG;
        }
        jobId = add_job(processId, state, cmdline);
        // if foreground job, wait; else continue.
        if (state == FG) {
            // wait for a SIGCHLD signal; the flag will change to 1
            while (!waitpidNeeded) {
                Sigsuspend(&prev);
            }
        } else {
            Sio_printf("[%d] (%d) %s\n", jobId, processId, cmdline);
        }
        Sigprocmask(SIG_SETMASK, &prev, NULL);
    }
    return;
}

/*****************
 * Signal handlers
 *****************/

/*
 * Handle SIGCHLD signals sent out by the kernel.
 * It reaps zombie child processes, handles stopped processes, 
 * and delete the corresponding job from the job list.
 *
 * @param SIGCHLD signal sent by kernel
 */
void sigchld_handler(int sig) {
    int oldErrno = errno;
    sigset_t mask, prev;
    jid_t jobId;
    pid_t processId;
    Sigfillset(&mask);
    int status;

    Sigprocmask(SIG_BLOCK, &mask, &prev);
    while ((processId = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        jobId = job_from_pid(processId);
        // change this volatile flag to notify parent
        if (fg_job() == jobId) {
            waitpidNeeded = 1;
        }

        if (WIFEXITED(status)) {
            // job terminated normally
            delete_job(jobId);
        } else if (WIFSTOPPED(status)) {
            // job stopped
            job_set_state(jobId, ST);
            Sio_printf("Job [%d] (%d) stopped by signal %d\n",
                       jobId, processId, WSTOPSIG(status));
        } else if (WIFSIGNALED(status)) {
            // job terminated by signal
            delete_job(jobId);
            Sio_printf("Job [%d] (%d) terminated by signal %d\n",
                       jobId, processId, WTERMSIG(status));
        }
    }
    Sigprocmask(SIG_SETMASK, &prev, NULL);
    errno = oldErrno;
    return;
}

/*
 * Handle Ctrl-C user input by sending a SIGINT signal
 * to each process in the foreground job.
 * It prints out an info message and terminates the process.
 *
 * @param sig signal sent by Ctrl-C
 */
void sigint_handler(int sig) {
    int oldErrno = errno;
    sigset_t mask, prev;
    jid_t jobId;
    pid_t processId;
    Sigfillset(&mask);

    Sigprocmask(SIG_BLOCK, &mask, &prev);
    jobId = fg_job();
    // if no job is in foreground, do nothing
    if (jobId == 0) {
        Sigprocmask(SIG_SETMASK, &prev, NULL);
        errno = oldErrno;
        return;
    }
    processId = job_get_pid(jobId);

    // send SIGINT signal
    Kill(-processId, SIGINT);
    Sigprocmask(SIG_SETMASK, &prev, NULL);
    errno = oldErrno;
    return;
}

/*
 * Handle Ctrl-Z user input by sending a SIGTSTP signal
 * to stop each process in foreground.
 * It prints out an info message and stops the process.
 *
 * @param sig signal sent by Ctrl-Z
 */
void sigtstp_handler(int sig) {
    int oldErrno = errno;
    sigset_t mask, prev;
    jid_t jobId;
    pid_t processId;
    Sigfillset(&mask);

    Sigprocmask(SIG_BLOCK, &mask, &prev);
    jobId = fg_job();

    // if no job is in foreground, do nothing
    if (jobId == 0) {
        Sigprocmask(SIG_SETMASK, &prev, NULL);
        errno = oldErrno;
        return;
    }
    processId = job_get_pid(jobId);

    // send SIGTSTP signal
    Kill(-processId, SIGTSTP);
    Sigprocmask(SIG_SETMASK, &prev, NULL);
    errno = oldErrno;
    return;
}

/*
 * cleanup - Attempt to clean up global resources when the program exits. In
 * particular, the job list must be freed at this time, since it may contain
 * leftover buffers from existing or even deleted jobs.
 */
void cleanup(void) {
    // Signals handlers need to be removed before destroying the joblist
    Signal(SIGINT, SIG_DFL);  // Handles Ctrl-C
    Signal(SIGTSTP, SIG_DFL); // Handles Ctrl-Z
    Signal(SIGCHLD, SIG_DFL); // Handles terminated or stopped child

    destroy_job_list();
}
