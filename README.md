# 📨 Servizio di Messaggistica TCP in C

Progetto universitario per il corso di **Sistemi Operativi** — implementazione di un servizio di messaggistica client-server su **TCP/IP** con server **multithread**, autenticazione utenti e persistenza su file, scritto interamente in C.

---

## 📌 Descrizione

Il sistema è composto da un **server concorrente** e un **client interattivo** che comunicano tramite socket TCP. Gli utenti possono registrarsi, autenticarsi, inviare messaggi ad altri utenti, leggere i messaggi ricevuti e cancellare quelli indesiderati. Ogni sessione client è gestita da un **thread dedicato** sul server.

---

## ✨ Funzionalità

- **Registrazione e login** con username e password
- **Invio messaggi** con campi Destinatario, Oggetto e Testo
- **Lettura messaggi** ricevuti con timestamp, mittente e contenuto
- **Cancellazione messaggi** tramite ID univoco
- **Verifica del destinatario** prima dell'invio (notifica se non esiste)
- **Validazione input** lato client: rifiuto di caratteri riservati (`|`, `\`)
- **Gestione segnali**: `SIGINT` per chiusura pulita del server, `SIGTSTP` ignorato su entrambi
- **Persistenza**: utenti e messaggi salvati su file di testo

---

## 🏗️ Architettura

```
┌─────────────┐    TCP / struct Packet    ┌──────────────────────┐
│   Client    │ ◄────────────────────►   │       Server         │
│  (client.c) │                          │     (server.c)       │
└─────────────┘                          │                      │
                                         │  Thread per client   │
                                         │  ┌────────────────┐  │
                                         │  │ handle_client  │  │
                                         │  └────────────────┘  │
                                         │                      │
                                         │  File di persistenza │
                                         │  ├── users.txt       │
                                         │  └── messages.txt    │
                                         └──────────────────────┘
```

### Componenti principali

**`server.c`** — server concorrente TCP:
- Accetta connessioni in loop e crea un thread per ogni client con `pthread_create` + `pthread_detach`
- Gestisce registrazione, login, invio, lettura e cancellazione messaggi
- Sincronizza l'accesso ai file tramite mutex dedicati (`file_mutex`, `register_mutex`, `auth_mutex`)
- Chiusura pulita su `SIGINT` tramite handler dedicato

**`client.c`** — client interattivo da terminale:
- Gestisce autenticazione iniziale (login o registrazione)
- Menu a scelte numeriche per tutte le operazioni
- Timeout su `recv` (200 ms) per ricevere la lista messaggi senza bloccarsi
- Genera ID univoco per ogni messaggio tramite combinazione di timestamp e valore pseudocasuale

**`common.h`** — definizioni condivise:
- Struttura `Packet` usata per tutti i messaggi del protocollo
- Tipi di pacchetto (`AUTH_REQUEST`, `AUTH_SUCCESS`, `AUTH_FAIL`, `SEND_MESSAGE`, `READ_MESSAGES`, `DELETE_MESSAGE`, `END_SESSION`, `CHECK_USER`, `MESSAGE_LIST`)
- Costanti di configurazione (`SERVER_PORT`, `MAX_USER`, `MAX_PASS`, `MAX_OBJ`, `MAX_BODY`, ecc.)

---

## 🔌 Protocollo di Comunicazione

Tutta la comunicazione avviene tramite scambio di strutture `Packet` di dimensione fissa. Il campo `type` determina il significato degli altri campi.

| `type`           | Direzione       | Descrizione                              |
|------------------|-----------------|------------------------------------------|
| `AUTH_REQUEST`   | Client → Server | Credenziali login o registrazione        |
| `AUTH_SUCCESS`   | Server → Client | Autenticazione riuscita                  |
| `AUTH_FAIL`      | Server → Client | Autenticazione fallita                   |
| `SEND_MESSAGE`   | Client → Server | Nuovo messaggio da inviare               |
| `CHECK_USER`     | Server → Client | Conferma/rifiuto esistenza destinatario  |
| `READ_MESSAGES`  | Client → Server | Richiesta lista messaggi                 |
| `MESSAGE_LIST`   | Server → Client | Un messaggio della lista (uno per invio) |
| `DELETE_MESSAGE` | Bidirezionale   | Richiesta/risposta cancellazione         |
| `END_SESSION`    | Client → Server | Disconnessione pulita                    |

---

## 💾 Persistenza

I dati sono memorizzati in due file di testo:

**`users.txt`** — un utente per riga:
```
username password
```

**`messages.txt`** — un messaggio per riga nel formato pipe-separated:
```
timestamp|id|sender|receiver|subject|body
```

I caratteri `|` e `\` sono vietati negli input dell'utente per non corrompere il formato.

---

## 🛠️ Compilazione ed Esecuzione

### Requisiti
- GCC
- Linux / macOS (POSIX)
- Libreria `pthread`

### Compilare

```bash
gcc server.c -o server -lpthread
gcc client.c -o client
```

### Avviare il server

```bash
./server
```

### Avviare il client

```bash
./client
```

> Premere `Ctrl+C` sul server per una chiusura pulita.

---

## 📁 Struttura del Repository

```
.
├── client.c        # Applicazione client interattiva
├── server.c        # Server concorrente multithread
├── common.h        # Strutture dati e costanti condivise
├── users.txt       # (generato a runtime) Archivio utenti registrati
└── messages.txt    # (generato a runtime) Archivio messaggi
```

---

## 🔒 Scelte Implementative

- **Mutex separati** per registrazione, autenticazione e I/O sui messaggi, per minimizzare la contesa tra thread.
- **Cancellazione sicura**: il server riscrive il file messaggi senza la riga da eliminare tramite file temporaneo + `rename`, operazione atomica a livello filesystem.
- **Timeout su recv**: il client imposta `SO_RCVTIMEO` a 200 ms per terminare la ricezione della lista messaggi senza un segnale esplicito di fine da parte del server.
- **Gestione `EINTR`**: tutti i cicli di lettura da file e da socket rilevano l'interruzione da segnale e riprovano l'operazione.
- **`pthread_detach`**: i thread client vengono staccati immediatamente dopo la creazione, evitando memory leak senza la necessità di un join esplicito.
