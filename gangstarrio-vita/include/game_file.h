#ifndef GANGSTARRIO_GAME_FILE_H
#define GANGSTARRIO_GAME_FILE_H

typedef enum GameFileStatus {
    GAME_FILE_OK = 0,
    GAME_FILE_MISSING,
    GAME_FILE_WRONG_SIZE,
    GAME_FILE_READ_ERROR,
    GAME_FILE_WRONG_HASH,
} GameFileStatus;

GameFileStatus game_file_verify(void);
const char *game_file_status_string(GameFileStatus status);

#endif

