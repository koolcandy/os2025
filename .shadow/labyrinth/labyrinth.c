#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <testkit.h>
#include "labyrinth.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    char *mapFile = NULL;
    char playerId = '\0';
    char *moveDirection = NULL;
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--map") == 0 || strcmp(argv[i], "-m") == 0) && i + 1 < argc) {
            mapFile = argv[i + 1];
            i++;
        } else if ((strcmp(argv[i], "--player") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            playerId = argv[i + 1][0];
            i++;
        } else if (strcmp(argv[i], "--move") == 0 && i + 1 < argc) {
            moveDirection = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("Labyrinth Game - Version 1.0\n");
            return 0;
        }
    }
    
    // 检查必要参数
    if (!mapFile || !playerId) {
        printUsage();
        return 1;
    }
    
    // 验证玩家ID
    if (!isValidPlayer(playerId)) {
        printf("Error: Invalid player ID. Must be a digit (0-9).\n");
        return 1;
    }
    
    // 加载地图
    Labyrinth labyrinth;
    if (!loadMap(&labyrinth, mapFile)) {
        printf("Error: Failed to load map from %s.\n", mapFile);
        return 1;
    }
    
    // 查找玩家位置
    Position playerPos = findPlayer(&labyrinth, playerId);
    
    // 如果玩家不在地图上，将其放置在第一个空位置
    if (playerPos.row == -1 && playerPos.col == -1) {
        playerPos = findFirstEmptySpace(&labyrinth);
        if (playerPos.row != -1 && playerPos.col != -1) {
            labyrinth.map[playerPos.row][playerPos.col] = playerId;
        } else {
            printf("Error: No empty space to place player.\n");
            return 1;
        }
    }
    
    // 如果指定了移动方向，则移动玩家
    if (moveDirection) {
        if (!movePlayer(&labyrinth, playerId, moveDirection)) {
            printf("Error: Invalid move.\n");
            return 1;
        }
        
        // 保存更新后的地图
        if (!saveMap(&labyrinth, mapFile)) {
            printf("Error: Failed to save map to %s.\n", mapFile);
            return 1;
        }
    }
    
    // 打印地图
    for (int i = 0; i < labyrinth.rows; i++) {
        printf("%s\n", labyrinth.map[i]);
    }
    
    return 0;
}