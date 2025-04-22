#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <getopt.h>
#include <testkit.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>

#define MAX_PID_NUM 327680

// Process structure
typedef struct {
    int pid;
    int ppid;
    char name[256];
} Process;

// Multi-branch tree node structure
typedef struct {
    Process process;
    ProcessNode **children;
    int children_count;
    int capacity;
} ProcessNode;

// Function prototypes
Process* get_proc_info(int* count);
ProcessNode* build_process_tree(Process* processes, int proc_count);
ProcessNode* find_init_process(Process* processes, int proc_count);
void print_process_tree(ProcessNode* node, bool show_pids, bool numeric_sort, int depth);
void free_process_tree(ProcessNode* node);

int main(int argc, char *argv[]) {

    // Default options
    bool show_pids = false;
    bool numeric_sort = false;
    char choice = 0;

    // Argument parsing
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--show-pids") == 0 || strcmp(argv[i], "-p") == 0) {
            show_pids = true;
        } else if (strcmp(argv[i], "--numeric-sort") == 0 || strcmp(argv[i], "-n") == 0) {
            numeric_sort = true;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            if (argc > 2) {
                printf("Error: --version option cannot be combined with other options.\n");
                return EXIT_FAILURE;
            } else {
                printf("pstree - Version 1.0\n");
                return EXIT_SUCCESS;
            }
        } else {
            printf("Unknown option: %s\n", argv[i]);
            printf("Usage: %s [--show-pids|-s] [--numeric-sort|-n] [--version|-v]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    // Get process information
    int proc_count = 0;
    Process* processes = get_proc_info(&proc_count);
    if (!processes || proc_count == 0) {
        fprintf(stderr, "Error: Failed to get process information\n");
        return EXIT_FAILURE;
    }
    
    // Build process tree
    ProcessNode* root = build_process_tree(processes, proc_count);
    
    // Print process tree
    print_process_tree(root, show_pids, numeric_sort, 0);
    printf("\n");
    
    // Clean up
    free_process_tree(root);
    free(processes);
    
    return EXIT_SUCCESS;
}

// Get process information from /proc directory
Process* get_proc_info(int* count) {
    // Allocate memory for process array dynamically
    Process* processes = malloc(MAX_PID_NUM * sizeof(Process));
    if (!processes) {
        perror("Failed to allocate memory for processes");
        exit(EXIT_FAILURE);
    }
    
    int index = 0;
    DIR* dir = opendir("/proc");
    if (dir == NULL) {
        perror("Failed to open /proc directory");
        free(processes);
        exit(EXIT_FAILURE);
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (isdigit(entry->d_name[0])) {
            // Get pid
            int pid = atoi(entry->d_name);
            processes[index].pid = pid;

            // Get ppid and name
            char path[256];
            snprintf(path, sizeof(path), "/proc/%d/stat", pid);
            FILE* file = fopen(path, "r");
            if (file == NULL) {
                // Skip this process if can't open file
                continue;
            }
            
            // Read the entire stat line
            char stat_line[1024];
            if (fgets(stat_line, sizeof(stat_line), file) == NULL) {
                fclose(file);
                continue;
            }
            fclose(file);
            
            // Extract process name (between first '(' and last ')')
            char* name_start = strchr(stat_line, '(');
            char* name_end = strrchr(stat_line, ')');
            if (name_start && name_end && name_start < name_end) {
                int name_len = name_end - name_start - 1;
                if (name_len >= sizeof(processes[index].name)) {
                    name_len = sizeof(processes[index].name) - 1;
                }
                strncpy(processes[index].name, name_start + 1, name_len);
                processes[index].name[name_len] = '\0';
            } else {
                strcpy(processes[index].name, "unknown");
            }
            
            // Extract ppid (4th field after the process name)
            char* ppid_ptr = name_end + 2;  // Skip past ')' and the space after it
            int ppid = 0;
            sscanf(ppid_ptr, "%*c %d", &ppid);
            processes[index].ppid = ppid;

            index++;
            if (index >= MAX_PID_NUM) {
                break;
            }
        }
    }

    closedir(dir);
    *count = index;
    
    return processes;
}

// Find the init process (PID 1 or the process with PPID 0)
ProcessNode* find_init_process(Process* processes, int proc_count) {
    ProcessNode* root = malloc(sizeof(ProcessNode));
    if (!root) {
        perror("Failed to allocate memory for root node");
        exit(EXIT_FAILURE);
    }
    
    // Look for PID 1 (init)
    for (int i = 0; i < proc_count; i++) {
        if (processes[i].pid == 1) {
            root->process = processes[i];
            root->children = NULL;
            root->children_count = 0;
            root->capacity = 0;
            return root;
        }
    }
    
    // If PID 1 not found, use first process with PPID 0
    for (int i = 0; i < proc_count; i++) {
        if (processes[i].ppid == 0) {
            root->process = processes[i];
            root->children = NULL;
            root->children_count = 0;
            root->capacity = 0;
            return root;
        }
    }
    
    // If no suitable process found, create a dummy process
    root->process.pid = 1;
    root->process.ppid = 0;
    strcpy(root->process.name, "init");
    root->children = NULL;
    root->children_count = 0;
    root->capacity = 0;
    
    return root;
}

// Build multi-branch tree of processes
ProcessNode* build_process_tree(Process* processes, int proc_count) {
    // Find or create the root process (init or PID 1)
    ProcessNode* root = find_init_process(processes, proc_count);
    
    // Allocate initial children array
    root->children = malloc(10 * sizeof(ProcessNode*));
    if (!root->children) {
        perror("Failed to allocate memory for children");
        free(root);
        exit(EXIT_FAILURE);
    }
    root->capacity = 10;
    root->children_count = 0;
    
    // Create a map to keep track of nodes by PID for O(1) lookups
    ProcessNode* node_map[MAX_PID_NUM] = {NULL};
    node_map[root->process.pid] = root;
    
    // First pass: create all nodes
    for (int i = 0; i < proc_count; i++) {
        if (processes[i].pid == root->process.pid) {
            continue; // Skip the root process
        }
        
        ProcessNode* node = malloc(sizeof(ProcessNode));
        if (!node) {
            perror("Failed to allocate process node");
            continue;
        }
        
        node->process = processes[i];
        node->children = malloc(10 * sizeof(ProcessNode*));
        if (!node->children) {
            perror("Failed to allocate children array");
            free(node);
            continue;
        }
        node->children_count = 0;
        node->capacity = 10;
        
        node_map[processes[i].pid] = node;
    }
    
    // Second pass: build the tree structure
    for (int i = 0; i < proc_count; i++) {
        if (processes[i].pid == root->process.pid) {
            continue; // Skip the root process
        }
        
        ProcessNode* child = node_map[processes[i].pid];
        ProcessNode* parent = node_map[processes[i].ppid];
        
        if (child && parent) {
            // Add child to parent's children list
            if (parent->children_count == parent->capacity) {
                parent->capacity *= 2;
                ProcessNode** new_children = realloc(parent->children, 
                                                parent->capacity * sizeof(ProcessNode*));
                if (!new_children) {
                    perror("Failed to reallocate children array");
                    continue;
                }
                parent->children = new_children;
            }
            parent->children[parent->children_count++] = child;
        } else if (child) {
            // If parent not found, add to root
            if (root->children_count == root->capacity) {
                root->capacity *= 2;
                ProcessNode** new_children = realloc(root->children, 
                                            root->capacity * sizeof(ProcessNode*));
                if (!new_children) {
                    perror("Failed to reallocate root children array");
                    continue;
                }
                root->children = new_children;
            }
            root->children[root->children_count++] = child;
        }
    }
    
    return root;
}

// Recursively print process tree with box-drawing characters
void print_process_tree(ProcessNode* node, bool show_pids, bool numeric_sort, int depth) {
    if (node == NULL) {
        return;
    }
    
    // Sort children if needed
    if (node->children_count > 0) {
        // Sort by PID if numeric sort requested
        if (numeric_sort) {
            for (int i = 0; i < node->children_count - 1; i++) {
                for (int j = 0; j < node->children_count - i - 1; j++) {
                    if (node->children[j]->process.pid > node->children[j + 1]->process.pid) {
                        ProcessNode* temp = node->children[j];
                        node->children[j] = node->children[j + 1];
                        node->children[j + 1] = temp;
                    }
                }
            }
        } else {
            // Sort by name alphabetically
            for (int i = 0; i < node->children_count - 1; i++) {
                for (int j = 0; j < node->children_count - i - 1; j++) {
                    if (strcmp(node->children[j]->process.name, node->children[j + 1]->process.name) > 0) {
                        ProcessNode* temp = node->children[j];
                        node->children[j] = node->children[j + 1];
                        node->children[j + 1] = temp;
                    }
                }
            }
        }
    }
    
    // Print process name
    if (depth > 0) {
        // For non-root nodes, print appropriate indentation
        for (int i = 0; i < depth - 1; i++) {
            printf("        │");
        }
        printf("        ├─");
    }
    
    printf("%s", node->process.name);
    
    // Show PID if requested
    if (show_pids) {
        printf("(%d)", node->process.pid);
    }
    
    // If this node has children, add the horizontal line
    if (node->children_count > 0) {
        printf("─┬─");
    }
    printf("\n");
    
    // Recursively print all children
    for (int i = 0; i < node->children_count; i++) {
        print_process_tree(node->children[i], show_pids, numeric_sort, depth + 1);
    }
    
    // Add a vertical line after the last child
    if (depth == 0 && node->children_count > 0) {
        printf("        │\n");
    }
}

// Free process tree memory
void free_process_tree(ProcessNode* node) {
    if (node == NULL) {
        return;
    }
    
    // Recursively free all child nodes
    for (int i = 0; i < node->children_count; i++) {
        free_process_tree(node->children[i]);
    }
    
    // Free children array and node itself
    free(node->children);
    free(node);
}