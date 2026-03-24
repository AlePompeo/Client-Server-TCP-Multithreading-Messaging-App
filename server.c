#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/types.h>
#include "common.h"
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include "common.h"

// Mutex per sincronizzare l'accesso ai file
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t register_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t auth_mutex = PTHREAD_MUTEX_INITIALIZER;

// Struttura per passare informazioni al thread
typedef struct {
    int sock;
    struct sockaddr_in addr;
} client_info;

// Variabile globale per controllare il ciclo del server
volatile sig_atomic_t server_running = 1;
int server_sock;

// Handler per SIGINT
void sigint_handler(int signo) {
    (void)signo;
    printf("\n[SERVER] Ricevuto SIGINT, chiusura in corso...\n");
    server_running = 0;
    close(server_sock);
}

// Funzioni per gestione utenti e messaggi
static int register_user(const char *user, const char *pass) {
    pthread_mutex_lock(&register_mutex);
    FILE *fp = fopen(USERS_FILE, "r");
    if (fp) {
        char u[MAX_USER], p[MAX_PASS];

    read_again:
        errno = 0;
        int ret = fscanf(fp, "%63s %63s", u, p);
        if (ret == EOF) {
            if (errno == EINTR) goto read_again;
            //file terminato: utente non presente, si può registrare
            fclose(fp);
            goto do_write;
        }
        if (ret == 2) {
            if (strcmp(u, user) == 0) {
                fclose(fp);
                pthread_mutex_unlock(&register_mutex);
                return 0;   //utente già esistente
            }
            goto read_again;   // continua alla riga successiva
        }
        fclose(fp);
    }
    if (!fp && errno != ENOENT) {
        perror("Errore apertura file utenti");
        pthread_mutex_unlock(&register_mutex);
        return 0;
    }

do_write:
    fp = fopen(USERS_FILE, "a");
    if (!fp) {
        perror("Errore apertura file utenti");
        pthread_mutex_unlock(&register_mutex);
        return 0;
    }
write_again:
    errno = 0;
    if (fprintf(fp, "%s %s\n", user, pass) < 0) {
        if (errno == EINTR) goto write_again;
        perror("fprintf");
    }
    fclose(fp);
    pthread_mutex_unlock(&register_mutex);
    return 1;
}

// Verifica se username e password corrispondono a un utente registrato
static int authenticate_user(const char *user, const char *pass) {
    pthread_mutex_lock(&auth_mutex);
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) {
        perror("Errore apertura file utenti");
        pthread_mutex_unlock(&auth_mutex);
        pthread_exit(NULL);
    }

    char u[MAX_USER], p[MAX_PASS];
auth_read_again:
    errno = 0;
    int ret = fscanf(fp, "%63s %63s", u, p);
    if (ret == EOF) {
        if (errno == EINTR) goto auth_read_again;
        fclose(fp);
        pthread_mutex_unlock(&auth_mutex);
        return 0;  // Utente non trovato
    }
    if (ret == 2) {
        if (strcmp(u, user) == 0 && strcmp(p, pass) == 0) {
            fclose(fp);
            pthread_mutex_unlock(&auth_mutex);
            return 1;
        }
        goto auth_read_again;  // Continua a leggere il prossimo utente
    }
    fclose(fp);
    pthread_mutex_unlock(&auth_mutex);
    return 0;
}

// Verifica se un utente esiste (usato per evitare duplicati in registrazione)
static int user_exists(const char *username) {
    pthread_mutex_lock(&register_mutex);
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) {
        perror("Errore apertura file utenti");
        pthread_mutex_unlock(&register_mutex);
        return 0;
    }
    char u[MAX_USER], p[MAX_PASS];

user_read_again:
    errno = 0;
    int ret = fscanf(fp, "%63s %63s", u, p);
    if (ret == EOF) {
        if (errno == EINTR) goto user_read_again;
        //file terminato senza trovare corrispondenza
        fclose(fp);
        pthread_mutex_unlock(&register_mutex);
        return 0;
    }
    if (ret == 2) {
        if (strcmp(u, username) == 0) {
            fclose(fp);
            pthread_mutex_unlock(&register_mutex);
            return 1;
        }
        goto user_read_again;   // continua alla riga successiva
    }
    // ret < 0 o valore inatteso: input malformato
    fclose(fp);
    pthread_mutex_unlock(&register_mutex);
    return 0;
}

// Salva messaggio nel formato:
// timestamp|id|sender|receiver|subject|body
static void save_message(Packet *pkt) {
    pthread_mutex_lock(&file_mutex);
    FILE *fp = fopen(MSG_FILE, "a");
    if (!fp) {
        perror("Errore apertura file messaggi");
        pthread_mutex_unlock(&file_mutex);
        return;
    }
write_again1:
    errno = 0;
    if(fprintf(fp, "%ld|%d|%s|%s|%s|%s\n", 
      (long)pkt->timestamp, 
            pkt->id, 
            pkt->sender, 
            pkt->receiver, 
            pkt->subject, 
            pkt->body) < 0) 
    {
        if (errno == EINTR) goto write_again1;
        perror("fprintf");

    }

    fclose(fp);
    pthread_mutex_unlock(&file_mutex); //aggiungi errno a printf

    // log console
    char tbuf[LOG_BUF];
    struct tm tm_info;
    if (localtime_r(&pkt->timestamp, &tm_info))
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_info);
    else
        strncpy(tbuf, "N/A", sizeof(tbuf));
    printf("[SAVE] id=%d %s | %s -> %s | %s\n",
        pkt->id, 
        tbuf, 
        pkt->sender, 
        pkt->receiver, 
        pkt->subject);
}

// Invia al client tutti i messaggi destinatario==user; riemette il campo id letto dal file
static void read_messages(const char *user, int sock) {
    pthread_mutex_lock(&file_mutex);
    FILE *fp = fopen(MSG_FILE, "r");
    if (!fp) {
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    Packet pkt;
    char line[MAX_LEN];
    while (fgets(line, sizeof(line), fp)) {
        long t;
        int id;
        char sender[MAX_USER], receiver[MAX_USER], subject[MAX_OBJ], body[MAX_BODY];
        // formato: %ld|%d|%[^|]|%[^|]|%[^|]|%[^\n]
        if (sscanf(line, "%ld|%d|%63[^|]|%63[^|]|%127[^|]|%1023[^\n]", &t, &id, sender, receiver, subject, body) == 6) {
            if (strcmp(receiver, user) == 0) {
                pkt.type = MESSAGE_LIST;
                pkt.timestamp = t;
                pkt.id = id;
                strncpy(pkt.sender, sender, MAX_USER-1); pkt.sender[MAX_USER-1]=0;
                strncpy(pkt.receiver, receiver, MAX_USER-1); pkt.receiver[MAX_USER-1]=0;
                strncpy(pkt.subject, subject, MAX_OBJ-1); pkt.subject[MAX_OBJ-1]=0;
                strncpy(pkt.body, body, MAX_BODY-1); pkt.body[MAX_BODY-1]=0;
                send(sock, &pkt, sizeof(pkt), 0);
            }
        } else {
            fprintf(stderr, "Warning: linea malformata ignorata durante read_messages: %s", line);
        }
    }
    fclose(fp);
    pthread_mutex_unlock(&file_mutex);
}

// Elimina un messaggio se id e destinatario corrispondono; riscrive il file senza la riga cancellata
static void delete_message(const char *user, int msg_id, int sock) {
    pthread_mutex_lock(&file_mutex);
    FILE *fp = fopen(MSG_FILE, "r");
    FILE *tmp = fopen(TMP_FILE, "w");
    if (!fp || !tmp) {
        if (fp) fclose(fp);
        if (tmp) fclose(tmp);
        pthread_mutex_unlock(&file_mutex);
        // invio fallimento
        Packet resp;
        resp.type = DELETE_MESSAGE;
        resp.id = msg_id;
        snprintf(resp.subject, MAX_OBJ, "ERR_IO");
        send(sock, &resp, sizeof(resp), 0);
        return;
    }

    char line[MAX_LEN];
    int deleted = 0;
    while (fgets(line, sizeof(line), fp)) {
        long t;
        int id;
        char sender[MAX_USER], receiver[MAX_USER], subject[MAX_OBJ], body[MAX_BODY];
        if (sscanf(line, "%ld|%d|%63[^|]|%63[^|]|%127[^|]|%1023[^\n]", &t, &id, sender, receiver, subject, body) == 6) {
            if (id == msg_id && strcmp(receiver, user) == 0) {
                // skip -> delete
                deleted = 1;
                char tbuf[64];
                struct tm tm_info;
                if (localtime_r((time_t*)&t, &tm_info))
                    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_info);
                else
                    strncpy(tbuf, "N/A", sizeof(tbuf));
                printf("[DELETE] id=%d %s | %s -> %s | %s\n", id, tbuf, sender, receiver, subject);
                continue;
            } else {
                fprintf(tmp, "%ld|%d|%s|%s|%s|%s\n", t, id, sender, receiver, subject, body); //riscrivo stringa su file
            }
        } else {
            // linea malformata: riscrivila per sicurezza
            fputs(line, tmp);
            fprintf(stderr, "Warning: linea malformata ignorata durante delete_message: %s", line);
        }
    }

    fclose(fp);
    fclose(tmp);

    if (deleted) {
        if (remove(MSG_FILE) != 0) {
            perror("remove");
        }
        if (rename(TMP_FILE, MSG_FILE) != 0) {
            perror("rename");
        }
    } else {
        // se non cancellato, rimuovo tmp e segnalo NOT_FOUND
        remove(TMP_FILE);
    }

    pthread_mutex_unlock(&file_mutex);

    Packet resp;
    resp.type = DELETE_MESSAGE;
    resp.id = msg_id;
    if (deleted) snprintf(resp.subject, MAX_OBJ, "OK");
    else snprintf(resp.subject, MAX_OBJ, "NOT_FOUND");
    send(sock, &resp, sizeof(resp), 0);
}

// Thread per gestire un client
static void *handle_client(void *arg) {
    client_info ci = *(client_info *)arg;
    free(arg);
    Packet pkt;
    char user[MAX_USER], pass[MAX_PASS];

    // AUTENTICAZIONE o REGISTRAZIONE
    recv(ci.sock, &pkt, sizeof(pkt), 0);
    if (pkt.type == AUTH_REQUEST) {
        strcpy(user, pkt.sender);
        strcpy(pass, pkt.body);

        if (strcmp(pkt.subject, "REGISTER") == 0) { 
            while (!register_user(user, pass)) {
                // utente già esistente: notifica il client e attendi nuove credenziali
                pkt.type = AUTH_FAIL;
                send(ci.sock, &pkt, sizeof(pkt), 0);

                // attendi un nuovo tentativo dal client
                if (recv(ci.sock, &pkt, sizeof(pkt), 0) <= 0) {
                    close(ci.sock);
                    pthread_exit(NULL);
                }
                strcpy(user, pkt.sender);
                strcpy(pass, pkt.body);
            }
            pkt.type = AUTH_SUCCESS;
            send(ci.sock, &pkt, sizeof(pkt), 0);
            printf("[REGISTRAZIONE] Utente %s registrato.\n", user);
        } else {
            if (authenticate_user(user, pass)) {
                pkt.type = AUTH_SUCCESS;
                send(ci.sock, &pkt, sizeof(pkt), 0);
                printf("[LOGIN] Utente %s autenticato.\n", user);
            } else {
                pkt.type = AUTH_FAIL;
                send(ci.sock, &pkt, sizeof(pkt), 0);
                printf("[LOGIN FALLITO] %s\n", user);
                close(ci.sock);
                pthread_exit(NULL);
            }
        }
    }

    // loop comandi
    while (1) {
        ssize_t r = recv(ci.sock, &pkt, sizeof(pkt), 0);
        if (r <= 0) {
            // client disconnesso o errore
            close(ci.sock);
            pthread_exit(NULL);
        }

        switch (pkt.type) {
            case SEND_MESSAGE:
                if (!user_exists(pkt.receiver)) {
                    Packet resp;
                    resp.type = CHECK_USER;
                    strncpy(resp.subject, "NO_RECEIVER", MAX_OBJ-1);
                    resp.subject[MAX_OBJ-1] = 0;
                    send(ci.sock, &resp, sizeof(resp), 0);
                    printf("[SEND] Destinatario '%s' non esiste, messaggio NON salvato.\n", pkt.receiver);
                    break;
                } else {
                    Packet resp;
                    resp.type = CHECK_USER;
                    strncpy(resp.subject, "AUTH_OK", MAX_OBJ-1);
                    resp.subject[MAX_OBJ-1] = 0;
                    send(ci.sock, &resp, sizeof(resp), 0);
                }
                // si assume che client abbia già generato pkt.id
                pkt.timestamp = time(NULL);
                save_message(&pkt);
                break;
            case READ_MESSAGES:
                read_messages(user, ci.sock);
                break;
            case DELETE_MESSAGE:
                delete_message(user, pkt.id, ci.sock);
                break;
            case END_SESSION:
                close(ci.sock);
                pthread_exit(NULL);
                break;
            default:
                // ignoriamo
                break;
        }
    }

    close(ci.sock);
    return NULL;
}

int main() {
    // Setup handler per SIGINT e ignorare SIGTSTP
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // Ripristina handler di default per tutti i segnali tranne SIGINT e ignora SIGTSTP
    struct sigaction sa_default;
    sa_default.sa_handler = SIG_DFL;
    sigemptyset(&sa_default.sa_mask);
    sa_default.sa_flags = 0;
    for (int signo = 1; signo < NSIG; ++signo) {
        if (signo != SIGINT)
            sigaction(signo, &sa_default, NULL);
    }

    // Ignora SIGTSTP (CTRL + Z)
    struct sigaction sa_ignore;
    sa_ignore.sa_handler = SIG_IGN;
    sigemptyset(&sa_ignore.sa_mask);
    sa_ignore.sa_flags = 0;
    sigaction(SIGTSTP, &sa_ignore, NULL);

    // Creazione socket
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind e listen
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }

    printf("Server in ascolto sulla porta %d... (CTRL + C per uscire)\n", SERVER_PORT);

    // Loop principale per accettare connessioni
    while (server_running) {
        client_info *ci = malloc(sizeof(client_info));
        if (!ci) {
            fprintf(stderr, "malloc failed\n");
            continue;
        }

        socklen_t len = sizeof(ci->addr);
        ci->sock = accept(server_sock, (struct sockaddr*)&ci->addr, &len);
        if (ci->sock < 0) {
            free(ci);
            if (!server_running) break; // uscita pulita
            fprintf(stderr, "accept failed\n");
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, ci) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            close(ci->sock);
            free(ci);
            continue;
        }
        pthread_detach(tid);
    }

    printf("[SERVER] Chiusura completata.\n");
    close(server_sock);
    return 0;
}

//gcc server.c -o server -lpthread