#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <ctype.h>

#define FUNCTION 0
#define EXPRESSION 1
#define MAX_FUNCTIONS 100

// Structure to store function information
typedef struct {
    char name[256];
    void* address;
    char return_type[64];
    char definition[1024];
    bool valid;
} FunctionInfo;

// Array to store multiple function pointers
FunctionInfo function_registry[MAX_FUNCTIONS];
int function_count = 0;

// Add global variables to store loaded function handles
void* current_handle = NULL;
void* current_function = NULL;

// Initialize function registry
void init_function_registry() {
    for (int i = 0; i < MAX_FUNCTIONS; i++) {
        function_registry[i].valid = false;
    }
}

// Find function in registry by name
FunctionInfo* find_function(const char* name) {
    for (int i = 0; i < function_count; i++) {
        if (function_registry[i].valid && strcmp(function_registry[i].name, name) == 0) {
            return &function_registry[i];
        }
    }
    return NULL;
}

// Add function to registry
bool add_function_to_registry(const char* name, void* address, const char* return_type, const char* definition) {
    if (function_count >= MAX_FUNCTIONS) {
        fprintf(stderr, "Function registry full\n");
        return false;
    }
    
    // Check if function already exists and update it
    FunctionInfo* existing = find_function(name);
    if (existing) {
        existing->address = address;
        strncpy(existing->return_type, return_type, sizeof(existing->return_type) - 1);
        strncpy(existing->definition, definition, sizeof(existing->definition) - 1);
        return true;
    }
    
    // Add new function
    int idx = function_count++;
    strncpy(function_registry[idx].name, name, sizeof(function_registry[idx].name) - 1);
    function_registry[idx].address = address;
    strncpy(function_registry[idx].return_type, return_type, sizeof(function_registry[idx].return_type) - 1);
    strncpy(function_registry[idx].definition, definition, sizeof(function_registry[idx].definition) - 1);
    function_registry[idx].valid = true;
    
    return true;
}

// Check if string is a function call (name followed by parentheses)
bool is_function_call(const char* str, char* function_name) {
    const char* open_paren = strchr(str, '(');
    if (!open_paren) return false;
    
    // Copy everything before the parenthesis into function_name
    int len = open_paren - str;
    strncpy(function_name, str, len);
    function_name[len] = '\0';
    
    // Trim whitespace
    char* end = function_name + len - 1;
    while (end > function_name && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    
    char* start = function_name;
    while (*start && isspace((unsigned char)*start)) start++;
    
    if (start != function_name)
        memmove(function_name, start, strlen(start) + 1);
        
    // Check if this function exists in our registry
    return find_function(function_name) != NULL;
}

// Improved function to check if input is a function definition
bool is_function_definition(const char* input) {
    // Skip whitespace
    while (*input && isspace(*input)) input++;
    
    // Should start with a type name (assume any word is a type)
    if (!isalpha(*input) && *input != '_') return false;
    
    // Skip type name
    while (*input && (isalnum(*input) || *input == '_')) input++;
    if (!*input) return false;
    
    // Skip whitespace after type
    while (*input && isspace(*input)) input++;
    if (!*input) return false;
    
    // Should have a function name
    if (!isalpha(*input) && *input != '_') return false;
    
    // Skip function name
    while (*input && (isalnum(*input) || *input == '_')) input++;
    if (!*input) return false;
    
    // Skip any whitespace
    while (*input && isspace(*input)) input++;
    
    // Should have opening parenthesis
    if (*input != '(') return false;
    
    // Find matching closing parenthesis
    int paren_count = 1;
    input++;
    while (*input && paren_count > 0) {
        if (*input == '(') paren_count++;
        if (*input == ')') paren_count--;
        input++;
    }
    if (paren_count > 0) return false; // Unmatched parenthesis
    
    // Skip whitespace after closing parenthesis
    while (*input && isspace(*input)) input++;
    
    // Should have opening brace
    return *input == '{';
}

// Execute a function by name with no arguments
bool execute_function(const char* name, int* result) {
    FunctionInfo* func = find_function(name);
    if (!func) {
        fprintf(stderr, "Function '%s' not found\n", name);
        return false;
    }
    
    // Currently only supports functions that return int and take no arguments
    if (strcmp(func->return_type, "int") == 0) {
        int (*func_ptr)() = (int (*)())func->address;
        *result = func_ptr();
        return true;
    }
    
    fprintf(stderr, "Only int return type is currently supported\n");
    return false;
}

// Create combined C file with all defined functions
char* create_combined_c_file() {
    char* temp_template = strdup("/tmp/combined_funcs_XXXXXX");
    if (!temp_template) {
        perror("strdup failed");
        return NULL;
    }
    
    int fd = mkstemp(temp_template);
    if (fd == -1) {
        perror("mkstemp failed");
        free(temp_template);
        return NULL;
    }
    
    char* final_filename = malloc(strlen(temp_template) + 3); // +3 for ".c\0"
    if (!final_filename) {
        perror("malloc failed");
        close(fd);
        free(temp_template);
        return NULL;
    }
    
    strcpy(final_filename, temp_template);
    strcat(final_filename, ".c");
    
    if (rename(temp_template, final_filename) == -1) {
        perror("rename failed");
        close(fd);
        unlink(temp_template);
        free(temp_template);
        free(final_filename);
        return NULL;
    }
    
    FILE* file = fdopen(fd, "w");
    if (!file) {
        perror("fdopen failed");
        close(fd);
        unlink(final_filename);
        free(temp_template);
        free(final_filename);
        return NULL;
    }
    
    // Add minimal headers
    fprintf(file, "#include <stdio.h>\n\n");
    
    // Add function declarations first to allow cross-referencing
    for (int i = 0; i < function_count; i++) {
        if (function_registry[i].valid) {
            fprintf(file, "%s %s();\n", function_registry[i].return_type, function_registry[i].name);
        }
    }
    
    fprintf(file, "\n");
    
    // Add function definitions
    for (int i = 0; i < function_count; i++) {
        if (function_registry[i].valid) {
            fprintf(file, "%s\n\n", function_registry[i].definition);
        }
    }
    
    fclose(file);
    free(temp_template);
    return final_filename;
}

char* c_template(bool type, const char* content) {
    int fd;
    
    // make tmp c code
    char* temp_template = strdup("/tmp/temp-code-XXXXXX");
    if (!temp_template) {
        perror("strdup failed");
        return NULL;
    }
    
    char* final_filename = malloc(strlen(temp_template) + 3); // +3 for ".c\0"
    if (!final_filename) {
        perror("malloc failed");
        free(temp_template);
        return NULL;
    }
    
    fd = mkstemp(temp_template);
    if (fd == -1) {
        perror("mkstemp failed");
        free(temp_template);
        free(final_filename);
        return NULL;
    }
    
    // rename the file to add suffix
    strcpy(final_filename, temp_template);
    strcat(final_filename, ".c");
    
    // rename the file
    if (rename(temp_template, final_filename) == -1) {
        perror("rename failed");
        close(fd);
        unlink(temp_template);
        free(temp_template);
        free(final_filename);
        return NULL;
    }
    
    FILE* file = fdopen(fd, "w");
    if (!file) {
        perror("fdopen failed");
        close(fd);
        unlink(final_filename);
        free(temp_template);
        free(final_filename);
        return NULL;
    }
    
    if (type == FUNCTION) {
        // Generate function.c file for function definition
        fprintf(file, "#include <stdio.h>\n");
        
        // Add forward declarations for all previously defined functions
        for (int i = 0; i < function_count; i++) {
            if (function_registry[i].valid) {
                fprintf(file, "%s %s();\n", function_registry[i].return_type, function_registry[i].name);
            }
        }
        
        fprintf(file, "\n%s\n", content);
    } else if (type == EXPRESSION) {
        // Generate expression.c file that returns the value of an expression
        fprintf(file, "#include <stdio.h>\n");
        
        // Add forward declarations for all functions
        for (int i = 0; i < function_count; i++) {
            if (function_registry[i].valid) {
                fprintf(file, "%s %s();\n", function_registry[i].return_type, function_registry[i].name);
            }
        }
        
        fprintf(file, "int __expr_wrapper() {\n");
        fprintf(file, "    return %s;\n", content);
        fprintf(file, "}\n");
        fprintf(file, "int main() {\n");
        fprintf(file, "    printf(\"%%d\\n\", __expr_wrapper());\n");
        fprintf(file, "    return 0;\n");
        fprintf(file, "}\n");
    } else {
        fprintf(stderr, "Unknown type\n");
        fclose(file);
        unlink(final_filename);
        free(temp_template);
        free(final_filename);
        return NULL;
    }
    
    fclose(file);
    free(temp_template);
    return final_filename;
}

int* create_pipe() {
    int* pipefd = malloc(2 * sizeof(int));
    if (!pipefd) {
        perror("malloc failed");
        return NULL;
    }
    if (pipe(pipefd) == -1) {
        perror("pipe");
        free(pipefd);
        return NULL;
    }
    return pipefd;
}

// Compile all functions together and load them
bool recompile_and_load_all_functions() {
    // Create combined C file with all defined functions
    char* c_code = create_combined_c_file();
    if (!c_code) {
        return false;
    }

    pid_t pid;
    int status;

    // First fork for compilation
    pid = fork();
    if (pid == -1) {
        perror("fork");
        free(c_code);
        return false;
    } else if (pid == 0) {
        // Child process for compilation
        execlp("gcc", "gcc", "-fPIC", "-shared", "-o", "/tmp/all_functions.so", c_code, NULL);
        perror("execlp gcc failed");
        exit(EXIT_FAILURE);
    }
    // Wait for compilation to complete
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid failed for compilation");
        free(c_code);
        return false;
    }
    // Check if compilation was successful
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Compilation failed with status %d\n", WEXITSTATUS(status));
        free(c_code);
        return false;
    }

    // Clean up previous handle if it exists
    if (current_handle) {
        dlclose(current_handle);
        current_handle = NULL;
    }

    // Load the compiled shared object with RTLD_GLOBAL to make symbols available
    current_handle = dlopen("/tmp/all_functions.so", RTLD_NOW | RTLD_GLOBAL);
    if (!current_handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        free(c_code);
        return false;
    }
    
    // Update function pointers in registry
    for (int i = 0; i < function_count; i++) {
        if (function_registry[i].valid) {
            void* func_ptr = dlsym(current_handle, function_registry[i].name);
            if (!func_ptr) {
                fprintf(stderr, "Warning: Could not find symbol for %s\n", function_registry[i].name);
            } else {
                function_registry[i].address = func_ptr;
            }
        }
    }
    
    free(c_code);
    return true;
}

// Compile a function definition and add it to registry
bool compile_and_load_function(const char* function_def) {
    // Check if we can compile the function individually first
    char* c_code = c_template(FUNCTION, function_def);
    if (!c_code) {
        return false;
    }

    pid_t pid;
    int status;

    // First fork for test compilation
    pid = fork();
    if (pid == -1) {
        perror("fork");
        free(c_code);
        return false;
    } else if (pid == 0) {
        // Child process for compilation (just to test if it compiles)
        execlp("gcc", "gcc", "-fPIC", "-c", "-o", "/tmp/temp_test.o", c_code, NULL);
        perror("execlp gcc test failed");
        exit(EXIT_FAILURE);
    }
    
    // Wait for test compilation to complete
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid failed for test compilation");
        free(c_code);
        return false;
    }
    
    // Check if test compilation was successful
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Function compilation failed with status %d\n", WEXITSTATUS(status));
        free(c_code);
        return false;
    }
    
    // Extract function name and return type
    char function_name[256] = {0};
    char return_type[64] = {0};
    
    // Skip any spaces or tabs at the beginning
    const char* start = function_def;
    while (*start == ' ' || *start == '\t') start++;
    
    // Parse return type
    int i = 0;
    while (*start && !isspace(*start) && i < sizeof(return_type) - 1)
        return_type[i++] = *start++;
    return_type[i] = '\0';
    
    // Skip whitespace between return type and function name
    while (*start && isspace(*start)) start++;
    
    // Parse function name (everything up to the opening parenthesis)
    i = 0;
    while (*start && *start != '(' && i < sizeof(function_name) - 1)
        function_name[i++] = *start++;
    function_name[i] = '\0';
    
    // Remove trailing spaces from function name
    char* end = function_name + strlen(function_name) - 1;
    while (end > function_name && (*end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }
    
    printf("Extracted function name: '%s'\n", function_name); // Debug output
    
    // Add the function to registry (temporarily with NULL address)
    if (!add_function_to_registry(function_name, NULL, return_type, function_def)) {
        fprintf(stderr, "Failed to add function to registry\n");
        free(c_code);
        return false;
    }
    
    // Recompile all functions together and load them
    if (!recompile_and_load_all_functions()) {
        // Remove the function from registry if recompilation fails
        function_count--;  // Crude way to remove the last added function
        free(c_code);
        return false;
    }
    
    free(c_code);
    return true;
}

// Evaluate an expression
bool evaluate_expression(const char* expression, int* result) {
    char* c_code = c_template(EXPRESSION, expression);
    if (!c_code) {
        return false;
    }
    
    pid_t pid;
    int status;

    // First fork for compilation - link with the functions library
    pid = fork();
    if (pid == -1) {
        perror("fork");
        free(c_code);
        return false;
    } else if (pid == 0) {
        // Child process for compilation
        if (function_count > 0) {
            // If we have functions, link with the shared library
            execlp("gcc", "gcc", "-o", "/tmp/temp_c_code", c_code, 
                   "-L/tmp", "-Wl,-rpath,/tmp", "/tmp/all_functions.so", NULL);
        } else {
            // No functions to link with
            execlp("gcc", "gcc", "-o", "/tmp/temp_c_code", c_code, NULL);
        }
        perror("execlp gcc failed");
        exit(EXIT_FAILURE);
    }

    // Wait for compilation to complete
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid failed for compilation");
        free(c_code);
        return false;
    }

    // Check if compilation was successful
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Compilation failed with status %d\n", WEXITSTATUS(status));
        free(c_code);
        return false;
    }

    // Create pipe before the second fork
    int* pipefd = create_pipe();
    if (!pipefd) {
        free(c_code);
        return false;
    }

    // Second fork for running the compiled program
    pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        free(c_code);
        return false;
    } else if (pid == 0) {
        // Child process
        close(pipefd[0]); // Close unused read end

        // Redirect stdout to pipe
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]); // Close the original fd after duplication

        // Set LD_LIBRARY_PATH to include /tmp
        setenv("LD_LIBRARY_PATH", "/tmp", 1);
        
        execlp("/tmp/temp_c_code", "/tmp/temp_c_code", NULL);
        perror("execlp program failed");
        exit(EXIT_FAILURE);
    }

    // Parent process
    close(pipefd[1]); // Close unused write end

    // Read result from the child process
    char buffer[128];
    ssize_t bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1);
    if (bytes_read == -1) {
        perror("read failed");
        close(pipefd[0]);
        free(c_code);
        return false;
    }
    buffer[bytes_read] = '\0'; // Null-terminate the string
    close(pipefd[0]);

    // Wait for child process to finish
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid failed");
        free(c_code);
        return false;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        *result = atoi(buffer);
        free(c_code);
        free(pipefd);
        return true;
    }

    fprintf(stderr, "Program execution failed\n");
    free(c_code);
    free(pipefd);
    return false;
}

int main() {
    char line[256];
    int result;
    
    // Initialize function registry
    init_function_registry();

    // while (1) {
    //     // read prompt
    //     printf("crepl> ");
    //     fflush(stdout);
    //     if (!fgets(line, sizeof(line), stdin)) {
    //         break;
    //     }
        
    //     // Remove trailing newline
    //     line[strcspn(line, "\n")] = 0;
        
    //     // Skip empty lines
    //     if (strlen(line) == 0) continue;
        
    //     // Check if this is a function definition first
    //     if (is_function_definition(line)) {
    //         if (compile_and_load_function(line)) {
    //             printf("Function defined.\n");
    //         } else {
    //             fprintf(stderr, "Failed to compile function.\n");
    //         }
    //     }
    //     // If not a function definition, try to handle as an expression
    //     else if (evaluate_expression(line, &result)) {
    //         printf("%d\n", result);
    //     }
    //     // If both fail, report an error
    //     else {
    //         fprintf(stderr, "Failed to evaluate expression or define function.\n");
    //     }
    // }
    
    // Clean up before exit
    if (current_handle) {
        dlclose(current_handle);
    }
    
    return 0;
}
