#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <regex.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_SYSCALLS 1024
#define TOP_N 5
#define LINE_BUFFER_SIZE 1024
#define REFRESH_INTERVAL_MS 100  // Display refresh interval in milliseconds

typedef struct {
    char name[64];
    double time;
    int calls;  // Count number of calls
} syscall_stat;

typedef struct {
    syscall_stat stats[MAX_SYSCALLS];
    int count;
    double total_time;
} syscall_stats;

// Global variables for cleanup
syscall_stats *g_stats = NULL;
int g_pipefd = -1;

// Signal handler for proper cleanup
void cleanup_handler(int sig) {
    fprintf(stderr, "\nReceived signal %d, cleaning up...\n", sig);
    if (g_stats) free(g_stats);
    if (g_pipefd >= 0) close(g_pipefd);
    exit(EXIT_FAILURE);
}

int parse_strace_line(char *line, char *syscall_name, double *time) {
    // Improved regex patterns for better matching
    const char *time_pattern = "<([0-9]+\\.[0-9]+)>";  // match <number.number>
    const char *syscall_pattern = "^([a-zA-Z_][a-zA-Z0-9_]*)\\(";  // match syscall name at start of line

    // Compile the time regex
    regex_t time_regex;
    int ret = regcomp(&time_regex, time_pattern, REG_EXTENDED);
    if (ret != 0) {
        char errbuf[100];
        regerror(ret, &time_regex, errbuf, sizeof(errbuf));
        fprintf(stderr, "Failed to compile time regex: %s\n", errbuf);
        return -1;
    }

    // Compile the syscall regex
    regex_t syscall_regex;
    ret = regcomp(&syscall_regex, syscall_pattern, REG_EXTENDED);
    if (ret != 0) {
        char errbuf[100];
        regerror(ret, &syscall_regex, errbuf, sizeof(errbuf));
        fprintf(stderr, "Failed to compile syscall regex: %s\n", errbuf);
        regfree(&time_regex);
        return -1;
    }

    // Match the syscall name
    regmatch_t syscall_match[2];
    if (regexec(&syscall_regex, line, 2, syscall_match, 0) == 0) {
        int name_len = syscall_match[1].rm_eo - syscall_match[1].rm_so;
        if (name_len >= 64) name_len = 63;  // Prevent buffer overflow
        snprintf(syscall_name, name_len + 1, "%.*s", name_len, line + syscall_match[1].rm_so);
        syscall_name[name_len] = '\0';  // Ensure null termination
    } else {
        regfree(&time_regex);
        regfree(&syscall_regex);
        return -1;  // Not a syscall line
    }

    // Match the time
    regmatch_t time_match[2];
    *time = 0.0;  // Default value if no time is found
    
    if (regexec(&time_regex, line, 2, time_match, 0) == 0) {
        int time_len = time_match[1].rm_eo - time_match[1].rm_so;
        char time_str[32] = {0};
        snprintf(time_str, sizeof(time_str), "%.*s", time_len, line + time_match[1].rm_so);
        *time = atof(time_str);
    }

    // Free the regex resources
    regfree(&time_regex);
    regfree(&syscall_regex);
    return 0;
}

void add_syscall(syscall_stats *stats, const char *name, double time) {
    if (!stats || !name) return;
    
    // Update total time
    stats->total_time += time;
    
    // Check if syscall already exists
    for (int i = 0; i < stats->count; i++) {
        if (strcmp(stats->stats[i].name, name) == 0) {
            stats->stats[i].time += time;
            stats->stats[i].calls++;
            return;
        }
    }

    // Add new syscall
    if (stats->count < MAX_SYSCALLS) {
        strncpy(stats->stats[stats->count].name, name, sizeof(stats->stats[0].name) - 1);
        stats->stats[stats->count].name[sizeof(stats->stats[0].name) - 1] = '\0';
        stats->stats[stats->count].time = time;
        stats->stats[stats->count].calls = 1;
        stats->count++;
    }
}

void print_top_syscalls(syscall_stats *stats, int n) {
    if (!stats || stats->count == 0) return;
    
    // Sort syscalls by time (using a simple bubble sort)
    for (int i = 0; i < stats->count - 1; i++) {
        for (int j = i + 1; j < stats->count; j++) {
            if (stats->stats[i].time < stats->stats[j].time) {
                syscall_stat temp = stats->stats[i];
                stats->stats[i] = stats->stats[j];
                stats->stats[j] = temp;
            }
        }
    }

    // Clear screen and move cursor to top-left
    printf("\033[2J\033[H");
    
    // Print header
    printf("=== Syscall Performance Statistics ===\n");
    printf("Total time: %.6f seconds\n\n", stats->total_time);
    printf("Top %d syscalls:\n", n);
    printf("%-20s %-12s %-10s %-10s\n", "Syscall", "Time (s)", "Calls", "% of Total");
    printf("------------------------------------------------------------------\n");
    
    // Print top N syscalls
    for (int i = 0; i < n && i < stats->count; i++) {
        double percent = (stats->total_time > 0) ? 
                        (stats->stats[i].time / stats->total_time * 100.0) : 0.0;
        printf("%-20s %-12.6f %-10d %-10.2f%%\n", 
               stats->stats[i].name, 
               stats->stats[i].time, 
               stats->stats[i].calls,
               percent);
    }
    printf("------------------------------------------------------------------\n");
    printf("Press Ctrl+C to exit...\n");
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Setup signal handlers for cleanup
    signal(SIGINT, cleanup_handler);
    signal(SIGTERM, cleanup_handler);

    int pipefd[2];
    
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    // Fork the current process
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);  // Close unused read end
        dup2(pipefd[1], STDERR_FILENO);  // Redirect stderr to pipe
        close(pipefd[1]);  // Close the original fd after duplication
        
        // Prepare arguments for strace
        char **strace_args = malloc((argc + 3) * sizeof(char *));
        if (!strace_args) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        
        strace_args[0] = "strace";
        strace_args[1] = "-T";  // Show syscall timing
        
        // Copy the command and its arguments
        for (int i = 1; i < argc; i++) {
            strace_args[i + 1] = argv[i];
        }

        // Redirect output to /dev/null
        strace_args[argc + 2] = ">";
        strace_args[argc + 3] = "/dev/null"; 
        strace_args[argc + 3] = NULL;

        // Try different paths for strace
        execvp("strace", strace_args);
        execv("/bin/strace", strace_args);
        execv("/usr/bin/strace", strace_args);
        
        // If we get here, execv failed
        perror("Failed to execute strace");
        free(strace_args);
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        close(pipefd[1]);  // Close unused write end
        g_pipefd = pipefd[0];  // Store for cleanup

        // Allocate and initialize syscall_stats
        g_stats = calloc(1, sizeof(syscall_stats));
        if (!g_stats) {
            perror("calloc");
            close(pipefd[0]);
            return EXIT_FAILURE;
        }

        char line[LINE_BUFFER_SIZE];
        int line_pos = 0;
        char syscall_name[64];
        double time;

        clock_t last_refresh = clock();
        
        // Read from pipe character by character
        char ch;
        while (read(pipefd[0], &ch, 1) > 0) {
            if (ch == '\n' || line_pos >= LINE_BUFFER_SIZE - 1) {
                line[line_pos] = '\0';
                line_pos = 0;

                // Check if strace process has completed
                if (strstr(line, "+++ exited")) {
                    break;
                }

                if (parse_strace_line(line, syscall_name, &time) == 0 && time > 0) {
                    add_syscall(g_stats, syscall_name, time);
                    
                    // Check if it's time to refresh the display
                    clock_t current = clock();
                    double elapsed_ms = (double)(current - last_refresh) * 1000.0 / CLOCKS_PER_SEC;
                    
                    if (elapsed_ms >= REFRESH_INTERVAL_MS) {
                        print_top_syscalls(g_stats, TOP_N);
                        last_refresh = current;
                    }
                }
            } else {
                line[line_pos++] = ch;
            }
        }
        
        // Final statistics display
        print_top_syscalls(g_stats, TOP_N);
        
        // Wait for child to avoid zombie process
        int status;
        waitpid(pid, &status, 0);
        
        // Cleanup
        free(g_stats);
        close(pipefd[0]);
    }

    return EXIT_SUCCESS;
}