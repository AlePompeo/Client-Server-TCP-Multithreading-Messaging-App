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

// Mutex per sincronizzare l'accesso a stdin
int ret;
pthread_mutex_t stdin_mutex = PTHREAD_MUTEX_INITIALIZER;

// Stampa un pacchetto ricevuto in formato leggibile
static void print_packet(const Packet *pkt) {
    char tbuf[PRINT_BUFLEN];
    struct tm tm_info;
    if (localtime_r(&pkt->timestamp, &tm_info))
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_info);
    else
        strncpy(tbuf, "N/A", sizeof(tbuf));

    printf("\n(ID %d) [%s] Da: %s\nOggetto: %s\nTesto: %s\n",
           pkt->id, 
           tbuf, 
           pkt->sender, 
           pkt->subject, 
           pkt->body
    );
}

// Genera un ID unico per il messaggio usando timestamp e random; non garantisce l'unicità assoluta ma riduce drasticamente le collisioni
static int generate_message_id() {
    return (int)((time(NULL) ^ rand()) % 1000000);
}

// Controlla se la stringa contiene caratteri proibiti ('|' o '\\') che potrebbero interferire con il protocollo di comunicazione
static int contains_forbidden(const char *s) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == '|' || s[i] == '\\') return 1;
    }
    return 0;
}

// Legge una stringa da stdin e verifica che non contenga caratteri proibiti; se l'input è invalido, chiede di reinserire
static void fgets_validated(char *buf, int size, const char *field_name) {
retry:
    if (!fgets(buf, size, stdin)) {
        fprintf(stderr, "Errore input %s\n", field_name);
        buf[0] = '\0';
        return;
    }
    buf[strcspn(buf, "\n")] = 0;
    if (contains_forbidden(buf)) {
        printf("Attenzione: il campo '%s' contiene caratteri non supportati ('|' o '\\').\n"
               "Reinserire il campo: ", field_name);
        goto retry;
    }
}

int main() {
    // Ignora segnali che potrebbero interrompere il client in modo non pulito
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);   // Ignora Ctrl+C
    sigaction(SIGTSTP, &sa, NULL);  // Ignora Ctrl+Z

    // Inizializza il generatore di numeri casuali con un seme più unico possibile
    srand((unsigned int)(time(NULL) ^ getpid() ^ (uintptr_t)&printf));

    // Crea socket e connetti al server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { 
        perror("socket"); 
        return 1; 
    }

    // Configura indirizzo server
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    // AUTENTICAZIONE o REGISTRAZIONE
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    char user[MAX_USER], pass[MAX_PASS];

retry_choice:
    int choice_auth;
    printf("\n1. Login\n2. Registrazione\nScelta: ");
    ret = scanf("%d", &choice_auth);
    if (ret == EOF && errno == EINTR) goto retry_choice;
    if (ret != 1) {
        fprintf(stderr, "Scelta non valida\n");
        while (getchar() != '\n');  
        goto retry_choice;
    }
    if((choice_auth == 1 || choice_auth == 2) == 0) {
        fprintf(stderr, "Scelta non valida\n");
        goto retry_choice;
    }

retry_user:
    printf("Username: ");
    ret = scanf("%63s", user);
    if (ret == EOF && errno == EINTR) goto retry_user;
    if (ret == 0) { printf("Input non valido\n"); close(sock); exit(-1); }
    if (strlen(user) == 0 || strlen(user) >= MAX_USER) {
        fprintf(stderr, "Username non valido\n");
        goto retry_user;
    }
    if (contains_forbidden(user)) {
        printf("Attenzione: username contiene caratteri non supportati ('|' o '\\'). Reinserire: \n");
        goto retry_user;
    }

retry_pass:
    printf("Password: ");
    ret = scanf("%63s", pass);
    if (ret == EOF && errno == EINTR) goto retry_pass;
    if (ret == 0) { printf("Input non valido\n"); close(sock); exit(-1); }
    if (strlen(pass) == 0 || strlen(pass) >= MAX_PASS) {
        fprintf(stderr, "Password non valida\n");
        goto retry_pass;
    }
    if (contains_forbidden(pass)) {
        printf("Attenzione: password contiene caratteri non supportati ('|' o '\\'). Reinserire: \n");
        goto retry_pass;
    }

    pkt.type = AUTH_REQUEST;
    strcpy(pkt.sender, user);
    strcpy(pkt.body, pass);
    if (choice_auth == 2) {
        strcpy(pkt.subject, "REGISTER");  // <--- flag per server
    } else {
        strcpy(pkt.subject, "LOGIN");
    }

    if (send(sock, &pkt, sizeof(pkt), 0) == -1) {
        perror("send");
        close(sock);
        return 1;
    }
    if (recv(sock, &pkt, sizeof(pkt), 0) == -1) {
        perror("recv");
        close(sock);
        return 1;
    }

    if (pkt.type == AUTH_FAIL) {
        if (choice_auth == 2) {
            printf("Utente già presente, scegli un altro username.\n");

        retry_user_reg:
            printf("Username: ");
            ret = scanf("%63s", user);
            if (ret == EOF && errno == EINTR) goto retry_user_reg;
            if (ret != 1) {
                printf("Input non valido\n");
                while (getchar() != '\n');
                goto retry_user_reg;
            }
            if (strlen(user) == 0 || strlen(user) >= MAX_USER) {
                fprintf(stderr, "Username non valido\n");
                goto retry_user_reg;
            }

        retry_pass_reg:
            printf("Password: ");
            ret = scanf("%63s", pass);
            if (ret == EOF && errno == EINTR) goto retry_pass_reg;
            if (ret != 1) {
                printf("Input non valido\n");
                while (getchar() != '\n');
                goto retry_pass_reg;
            }
            if (strlen(pass) == 0 || strlen(pass) >= MAX_PASS) {
                fprintf(stderr, "Password non valida\n");
                goto retry_pass_reg;
            }

            memset(&pkt, 0, sizeof(Packet));
            pkt.type = AUTH_REQUEST;
            strcpy(pkt.sender, user);
            strcpy(pkt.body, pass);
            strcpy(pkt.subject, "REGISTER");

            if (send(sock, &pkt, sizeof(pkt), 0) == -1) { perror("send"); close(sock); return 1; }
            if (recv(sock, &pkt, sizeof(pkt), 0) == -1) { perror("recv"); close(sock); return 1; }

            if (pkt.type == AUTH_FAIL) {
                printf("Utente già presente, scegli un altro username.\n");
                goto retry_user_reg;
            }
        } else {
            printf("Autenticazione fallita, riavvia il programma per riprovare.\n");
            close(sock);
            return 0;
        }
    }

    printf("Accesso riuscito come %s\n", user);

    int choice;
    while (1) {
        printf("\n--- Menu ---\n");
        printf("1. Invia messaggio\n");
        printf("2. Leggi messaggi\n");
        printf("3. Cancella messaggio (usando ID)\n");
        printf("4. Esci\nScelta: ");
retry_num:
        ret = scanf("%d", &choice);
        if (ret == EOF && errno == EINTR) goto retry_num;
        if (ret != 1) {
            fprintf(stderr, "Scelta non valida\n");
            while (getchar() != '\n');  
            goto retry_num;
        }
        getchar();

        if (choice == 1) {
            Packet out;
            memset(&out, 0, sizeof(Packet));
            out.type = SEND_MESSAGE;
            strncpy(out.sender, user, MAX_USER-1); out.sender[MAX_USER-1] = 0;

            pthread_mutex_lock(&stdin_mutex);

            printf("Destinatario: ");
            fgets_validated(out.receiver, MAX_USER, "Destinatario");

            if (strcmp(out.receiver, user) == 0) {
                printf("Attenzione: il destinatario corrisponde all'utente loggato.\n");
            }

            printf("Oggetto: ");
            fgets_validated(out.subject, MAX_OBJ, "Oggetto");

            printf("Testo: ");
            fgets_validated(out.body, MAX_BODY, "Testo");

            pthread_mutex_unlock(&stdin_mutex);

            out.timestamp = time(NULL);
            out.id = generate_message_id();

            if (send(sock, &out, sizeof(out), 0) == -1) {
                perror("send");
                continue;
            }

            Packet resp;
            memset(&resp, 0, sizeof(Packet));
            if (recv(sock, &resp, sizeof(resp), 0) > 0) {
                if (resp.type == CHECK_USER && strcmp(resp.subject, "NO_RECEIVER") == 0) {
                    printf("Errore: destinatario non esistente!\n");
                    continue;
                }
                if (resp.type == CHECK_USER && strcmp(resp.subject, "AUTH_OK") == 0) {
                    printf("Destinatario esistente, invio messaggio...\n");
                }
            }

            printf("Messaggio inviato con id=%d\n", out.id);
        }
        else if (choice == 2) {
            Packet in;
            memset(&in, 0, sizeof(Packet));
            in.type = READ_MESSAGES;
            if (send(sock, &in, sizeof(in), 0) == -1) {
                perror("send");
                continue;
            }

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000; // 200 ms
            if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) == -1) {
                perror("setsockopt");
                continue;
            }

            printf("\n-- Messaggi ricevuti --\n");
            int msg_count = 0;
            while (1) {
                ssize_t r = recv(sock, &in, sizeof(in), 0);
                if (r == -1) {
                    if (errno == EWOULDBLOCK || errno == EAGAIN) break;
                    perror("recv");
                    break;
                }
                if (r == 0) break;
                if (in.type == MESSAGE_LIST) {
                    print_packet(&in);
                    msg_count++;
                }
            }
            if (msg_count == 0) printf("Nessun messaggio da leggere.\n");
            // reset timeout (opzionale)
            tv.tv_sec = 0; tv.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        }
        else if (choice == 3) {
            Packet in;
            memset(&in, 0, sizeof(Packet));  
            in.type = READ_MESSAGES;
            if (send(sock, &in, sizeof(in), 0) == -1) {
                perror("send");
                continue;
            }

            struct timeval tv;
            tv.tv_sec = 0; tv.tv_usec = 200000;
            if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) == -1) {
                perror("setsockopt");
                continue;
            }
            printf("\n-- Messaggi --\n");
            while (1) {
                ssize_t r = recv(sock, &in, sizeof(in), 0);
                if (r == -1) {
                    if (errno == EWOULDBLOCK || errno == EAGAIN) break;
                    perror("recv");
                    break;
                }
                if (r == 0) break;
                if (in.type == MESSAGE_LIST) print_packet(&in);
            }
            tv.tv_sec = 0; tv.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

            printf("\nInserisci ID messaggio da cancellare: ");
            int id;
retry_id:
            ret = scanf("%d", &id);
            if (ret == EOF && errno == EINTR) goto retry_id;
            if (ret != 1) {
                fprintf(stderr, "Input non valido, inserisci un numero\n");
                while (getchar() != '\n');
                goto retry_id;
            }
            getchar();

            Packet del;
            memset(&del, 0, sizeof(Packet));  
            del.type = DELETE_MESSAGE;
            del.id = id;
            if (send(sock, &del, sizeof(del), 0) == -1) {
                perror("send");
                continue;
            }

            // attendo risposta (bloccante)
            Packet resp;
            memset(&resp, 0, sizeof(Packet));
            if (recv(sock, &resp, sizeof(resp), 0) > 0 && resp.type == DELETE_MESSAGE) {
                if (strcmp(resp.subject, "OK") == 0) printf("Messaggio id=%d cancellato.\n", id);
                else printf("Messaggio id=%d non trovato o non eliminabile.\n", id);
            } else {
                printf("Errore ricezione risposta cancellazione.\n");
            }
        }
        else if (choice == 4) {
            Packet end;
            memset(&end, 0, sizeof(Packet)); 
            end.type = END_SESSION;
            if (send(sock, &end, sizeof(end), 0) == -1) {
                perror("send");
            }
            close(sock);
            break;
        }
        else {
            printf("\n\n\n");
            printf("Scelta non valida.\n");
            printf("\n\n\n");
        }
    }

    close(sock);
    return 0;
}

//gcc client.c -o client