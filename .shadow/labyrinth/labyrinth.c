#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <getopt.h>
#include <testkit.h>
#include "labyrinth.h"

int main(int argc, char *argv[]) {
    char *mapFile = NULL;
    char playerId = '\0';
    char *moveDirection = NULL;
    
    static struct option long_options[] = {
        {"map",     required_argument, 0, 'm'},
        {"player",  required_argument, 0, 'p'},
        {"move",    required_argument, 0, 'x'},
        {"version", no_argument,       0, 'v'},
        {0,         0,                 0,  0 }
    };

    int option_index = 0;
    int c;

    // Parse command line arguments using getopt_long
    while ((c = getopt_long(argc, argv, "m:p:v", long_options, &option_index)) != -1) {
        switch (c) {
            case 'm':
                mapFile = optarg;
                break;
            case 'p':
                playerId = optarg[0];
                break;
            case 'M':
                moveDirection = optarg;
                break;
            case 'v':
                printf("Labyrinth Game - Version 1.0\n");
                return 0;
            case '?':
                // getopt_long already printed an error message
                printUsage();
                return 1;
            default:
                abort();
        }
    }

    // Check for required parameters
    if (!mapFile || !playerId) {
        printUsage();
        return 1;
    }
    
    // Validate player ID
    if (!isValidPlayer(playerId)) {
        printf("Error: Invalid player ID. Must be a digit (0-9).\n");
        return 1;
    }
    
    // Load map
    Labyrinth labyrinth;
    if (!loadMap(&labyrinth, mapFile)) {
        printf("Error: Failed to load map from %s.\n", mapFile);
        return 1;
    }
    
    // Find player position
    Position playerPos = findPlayer(&labyrinth, playerId);
    
    // If player is not on the map, place them in the first empty space
    if (playerPos.row == -1 && playerPos.col == -1) {
        playerPos = findFirstEmptySpace(&labyrinth);
        if (playerPos.row != -1 && playerPos.col != -1) {
            labyrinth.map[playerPos.row][playerPos.col] = playerId;
        } else {
            printf("Error: No empty space to place player.\n");
            return 1;
        }
    }
    
    // If a move direction is specified, move the player
    if (moveDirection) {
        if (!movePlayer(&labyrinth, playerId, moveDirection)) {
            printf("Error: Invalid move.\n");
            return 1;
        }
        
        // Save the updated map
        if (!saveMap(&labyrinth, mapFile)) {
            printf("Error: Failed to save map to %s.\n", mapFile);
            return 1;
        }
    }
    
    // Print the map
    for (int i = 0; i < labyrinth.rows; i++) {
        printf("%s\n", labyrinth.map[i]);
    }
    
    return 0;
}

void printUsage() {
    printf("Usage:\n");
    printf("  labyrinth --map map.txt --player id\n");
    printf("  labyrinth -m map.txt -p id\n");
    printf("  labyrinth --map map.txt --player id --move direction\n");
    printf("  labyrinth --version\n");
}

bool isValidPlayer(char playerId) {
    return playerId >= '0' && playerId <= '9';
}

bool loadMap(Labyrinth *labyrinth, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        return false;
    }
    
    char line[MAX_COLS + 2]; // +2 to leave space for newline and null terminator
    int row = 0;
    int expectedCols = -1;
    
    while (fgets(line, sizeof(line), file) && row < MAX_ROWS) {
        // Remove newline character
        size_t len = strlen(line);
        if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        
        // Check if each line has the same length
        if (expectedCols == -1) {
            expectedCols = len;
        } else if (len != expectedCols) {
            fclose(file);
            return false; // Inconsistent line length
        }
        
        // Check if line length is within range
        if (len > MAX_COLS) {
            fclose(file);
            return false; // Map too large
        }
        
        // Copy to map
        strcpy(labyrinth->map[row], line);
        row++;
    }
    
    labyrinth->rows = row;
    labyrinth->cols = expectedCols;
    
    fclose(file);
    
    // Check map validity
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            char c = labyrinth->map[i][j];
            if (c != '#' && c != '.' && !(c >= '0' && c <= '9')) {
                return false; // Invalid character
            }
        }
    }
    
    // Check connectivity
    if (!isConnected(labyrinth)) {
        return false;
    }
    
    return row > 0 && expectedCols > 0; // Ensure map is not empty
}

Position findPlayer(Labyrinth *labyrinth, char playerId) {
    Position pos = {-1, -1};
    
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            if (labyrinth->map[i][j] == playerId) {
                pos.row = i;
                pos.col = j;
                return pos;
            }
        }
    }
    
    return pos;
}

Position findFirstEmptySpace(Labyrinth *labyrinth) {
    Position pos = {-1, -1};
    
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            if (labyrinth->map[i][j] == '.') {
                pos.row = i;
                pos.col = j;
                return pos;
            }
        }
    }
    
    return pos;
}

bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    // Check boundaries
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) {
        return false;
    }
    
    return labyrinth->map[row][col] == '.';
}

bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    // Find the player's current position
    Position pos = findPlayer(labyrinth, playerId);
    if (pos.row == -1 || pos.col == -1) {
        return false; // Player not on the map
    }
    
    int newRow = pos.row;
    int newCol = pos.col;
    
    // Calculate the new position
    if (strcmp(direction, "up") == 0) {
        newRow--;
    } else if (strcmp(direction, "down") == 0) {
        newRow++;
    } else if (strcmp(direction, "left") == 0) {
        newCol--;
    } else if (strcmp(direction, "right") == 0) {
        newCol++;
    } else {
        return false; // Invalid direction
    }
    
    // Check if the new position is valid
    if (newRow < 0 || newRow >= labyrinth->rows || newCol < 0 || newCol >= labyrinth->cols) {
        return false; // Out of bounds
    }
    
    // Check if the new position is movable
    char target = labyrinth->map[newRow][newCol];
    if (target == '#' || (target >= '0' && target <= '9')) {
        return false; // Wall or other player
    }
    
    // Move the player
    labyrinth->map[pos.row][pos.col] = '.';
    labyrinth->map[newRow][newCol] = playerId;
    
    return true;
}

bool saveMap(Labyrinth *labyrinth, const char *filename) {
    FILE *file = fopen(filename, "w");
    if (!file) {
        return false;
    }
    
    for (int i = 0; i < labyrinth->rows; i++) {
        fprintf(file, "%s\n", labyrinth->map[i]);
    }
    
    fclose(file);
    return true;
}

// Check if all empty spaces are connected using DFS
void dfs(Labyrinth *labyrinth, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    // Check boundaries
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) {
        return;
    }
    
    // Check if it's a wall or already visited
    char cell = labyrinth->map[row][col];
    if (cell == '#' || visited[row][col]) {
        return;
    }
    
    // Mark as visited
    visited[row][col] = true;
    
    // Explore four directions
    dfs(labyrinth, row + 1, col, visited); // Down
    dfs(labyrinth, row - 1, col, visited); // Up
    dfs(labyrinth, row, col + 1, visited); // Right
    dfs(labyrinth, row, col - 1, visited); // Left
}

bool isConnected(Labyrinth *labyrinth) {
    bool visited[MAX_ROWS][MAX_COLS] = {false};
    Position start = {-1, -1};
    
    // Find the first empty space or player as the starting point
    for (int i = 0; i < labyrinth->rows && start.row == -1; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            char cell = labyrinth->map[i][j];
            if (cell == '.' || (cell >= '0' && cell <= '9')) {
                start.row = i;
                start.col = j;
                break;
            }
        }
    }
    
    // If there are no empty spaces or players, the map is invalid
    if (start.row == -1) {
        return true; // An empty map is considered connected by default
    }
    
    // Start DFS from the starting point
    dfs(labyrinth, start.row, start.col, visited);
    
    // Check if all non-wall positions have been visited
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            char cell = labyrinth->map[i][j];
            if ((cell == '.' || (cell >= '0' && cell <= '9')) && !visited[i][j]) {
                return false; // Found an unvisited non-wall position
            }
        }
    }
    
    return true;
}