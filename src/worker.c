#include <sy5/worker.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/fcntl.h>
#include <sy5/utils.h>
#include <sy5/array.h>

worker **g_workers = NULL;
uint64_t *g_running_taskids = NULL;

int create_worker(worker **dest, task task) {
    worker *tmp = malloc(sizeof(worker));
    assert(tmp);
    
    tmp->task = task;
    tmp->runs = NULL;
    tmp->last_stdout.length = 0;
    tmp->last_stdout.data = NULL;
    tmp->last_stderr.length = 0;
    tmp->last_stderr.data = NULL;
    
    *dest = tmp;
    
    return 0;
}

int free_worker(worker **worker) {
    free_task(&(*worker)->task);
    array_free((*worker)->runs);
    free_string(&(*worker)->last_stdout);
    free_string(&(*worker)->last_stderr);
    free(*worker);
    *worker = NULL;
    
    return 0;
}

bool is_worker_running(uint64_t taskid) {
    bool alive = false;
    
    for (uint64_t i = 0; i < array_size(g_running_taskids); i++) {
        if (g_running_taskids[i] == taskid) {
            alive = true;
            break;
        }
    }
    
    return alive;
}

int remove_worker(uint64_t taskid) {
    for (uint64_t i = 0; i < array_size(g_running_taskids); i++) {
        if (g_running_taskids[i] == taskid) {
            array_remove(g_running_taskids, i);
            break;
        }
    }
    
    return 0;
}

worker *get_worker(uint64_t taskid) {
    for (uint64_t i = 0; i < array_size(g_workers); i++) {
        if (g_workers[i]->task.taskid == taskid) {
            return g_workers[i];
        }
    }
    
    return NULL;
}

void *worker_main(void *worker_arg) {
    worker *worker_to_handle = (worker *)worker_arg;
    timing timing = worker_to_handle->task.timing;
    
    uint64_t execution_time;
    time_t timestamp;
    struct tm *time_info;
    
    while (true) {
        execution_time = time(NULL);
        timestamp = (time_t)execution_time;
        time_info = localtime(&timestamp);
        if (((timing.daysofweek >> time_info->tm_wday) % 2 == 0) ||
            ((timing.hours >> time_info->tm_hour) % 2 == 0) ||
            ((timing.minutes >> time_info->tm_min) % 2 == 0)) {
            usleep(1000000 * (60 - time_info->tm_sec));
            continue;
        }
        
        if (!is_worker_running(worker_to_handle->task.taskid)) {
            break;
        }
        
        int stdout_pipe[2];
        int stderr_pipe[2];
        fatal_assert(pipe(stdout_pipe) != -1, "cannot create stdout pipe!\n");
        fatal_assert(pipe(stderr_pipe) != -1, "cannot create stdout pipe!\n");
        
        pid_t fork_pid = fork();
        fatal_assert(fork_pid != -1, "cannot create task fork!\n");
        
        if (fork_pid == 0) {
            fatal_assert(close(stdout_pipe[0]) != -1, "cannot close stdout pipe!\n");
            fatal_assert(close(stderr_pipe[0]) != -1, "cannot close stderr pipe!\n");
            fatal_assert(dup2(stdout_pipe[1], STDOUT_FILENO) != -1, "cannot duplicate stdout!\n");
            fatal_assert(dup2(stderr_pipe[1], STDERR_FILENO) != -1, "cannot duplicate stderr!\n");
            fatal_assert(close(stdout_pipe[1]) != -1, "cannot close stdout pipe!\n");
            fatal_assert(close(stderr_pipe[1]) != -1, "cannot close stderr pipe!\n");
            
            char **argv = NULL;
            for (uint32_t i = 0; i < worker_to_handle->task.commandline.argc; i++) {
                char *arg = NULL;
                fatal_assert(cstring_from_string(&arg, worker_to_handle->task.commandline.argv) != -1, "cannot convert cstring from string!\n");
                fatal_assert(array_push(argv, arg) != -1, "cannot push arg to argv!\n");
            }
            char *null_arg = NULL;
            fatal_assert(array_push(argv, null_arg) != -1, "cannot push arg to argv!\n");
            
            execvp(argv[0], argv);
            perror("execve");
            exit(EXIT_FAILURE);
        }
        
        fatal_assert(close(stdout_pipe[1]) != -1, "cannot close stdout pipe!\n");
        fatal_assert(close(stderr_pipe[1]) != -1, "cannot close stderr pipe!\n");
        
        char buf[PIPE_BUF] = { 0 };
        
        char *stdout_buf = NULL;
        while (read(stdout_pipe[0], buf, sizeof(buf)) != 0) {
            for (uint32_t i = 0; i < PIPE_BUF; i++) {
                array_push(stdout_buf, buf[i]);
                
                if (buf[i] == 0) {
                    break;
                }
            }
            
            memset (buf, 0, sizeof(buf));
        }
        
        char *stderr_buf = NULL;
        while (read(stderr_pipe[0], buf, sizeof(buf)) != 0) {
            for (uint32_t i = 0; i < PIPE_BUF; i++) {
                array_push(stderr_buf, buf[i]);
                
                if (buf[i] == 0) {
                    break;
                }
            }
            
            char null_char = 0;
            array_push(stderr_buf, null_char);
            
            memset (buf, 0, sizeof(buf));
        }
    
        fatal_assert(close(stdout_pipe[0]) != -1, "cannot close stdout pipe!\n");
        fatal_assert(close(stderr_pipe[0]) != -1, "cannot close stderr pipe!\n");
        
        int status;
        fatal_assert(waitpid(fork_pid, &status, 0) != -1, "cannot waitpid for fork!\n");
        
        if (stdout_buf != NULL) {
            string_from_cstring(&worker_to_handle->last_stdout, stdout_buf);
            array_free(stdout_buf);
        }
        
        if (stderr_buf != NULL) {
            string_from_cstring(&worker_to_handle->last_stderr, stderr_buf);
            array_free(stderr_buf);
        }
        
        run cur_run = {
            .exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : 0xFFFF,
            .time = execution_time
        };
        
        array_push(worker_to_handle->runs, cur_run);
        
        execution_time = time(NULL);
        timestamp = (time_t)execution_time;
        time_info = localtime(&timestamp);
        usleep(1000000 * (60 - time_info->tm_sec));
    }
    
    goto cleanup;
    
    error:
    log("error in worker thread!\n");
    
    cleanup:
    free_worker(&worker_to_handle);
    
    return NULL;
}