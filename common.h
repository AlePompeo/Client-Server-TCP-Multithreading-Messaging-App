#ifndef COMMON_H
#define COMMON_H

#include <time.h>

#define SERVER_PORT 9090
#define MAX_CLIENTS 50
#define MAX_LEN 1024
#define MAX_USER 64
#define MAX_PASS 64
#define MAX_OBJ 128
#define MAX_BODY 1024
#define PRINT_BUFLEN 64
#define LOG_BUF 64

#define USERS_FILE "users.txt"
#define MSG_FILE "messages.txt"
#define TMP_FILE "messages_tmp.txt"

typedef enum {
    AUTH_REQUEST,
    AUTH_SUCCESS,
    AUTH_FAIL,
    SEND_MESSAGE,
    READ_MESSAGES,
    CHECK_USER,
    DELETE_MESSAGE,
    MESSAGE_LIST,
    END_SESSION
} PacketType;

typedef struct {
    PacketType type;
    char sender[MAX_USER];
    char receiver[MAX_USER];
    char subject[MAX_OBJ];
    char body[MAX_BODY];
    time_t timestamp;
    int id;  // id messaggio per cancellazione
} Packet;

#endif
//sudo fuser -k 8080/tcp

