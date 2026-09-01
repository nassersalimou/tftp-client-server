#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>

#define TFTP_PORT 69
#define TFTP_BLOCK_SIZE 512
#define TIMEOUT 5  // Timeout en secondes
#define MAX_RETRIES 5

// Opcodes TFTP
#define OPCODE_RRQ 1
#define OPCODE_WRQ 2
#define OPCODE_DATA 3
#define OPCODE_ACK 4
#define OPCODE_ERROR 5

typedef struct {
    uint16_t opcode;
    char filename[256];
    char mode[10];
} TFTP_Request;

typedef struct {
    uint16_t opcode;
    uint16_t blocknum;
    char data[TFTP_BLOCK_SIZE];
} TFTP_DataPacket;

typedef struct {
    uint16_t opcode;
    uint16_t blocknum;
} TFTP_AckPacket;

typedef struct {
    uint16_t opcode;
    uint16_t errorCode;
    char errorMsg[256];
} TFTP_ErrorPacket;

// Envoi d'un paquet d'erreur TFTP
void send_error(int sock, struct sockaddr_in *client_addr, socklen_t client_len, uint16_t errorCode, const char *errorMsg) {
    char buffer[516];
    uint16_t opcode = htons(OPCODE_ERROR);  // L'opcode pour l'erreur est 5
    uint16_t errCode = htons(errorCode);  // Conversion du code d'erreur en format réseau
    int msgLen = strlen(errorMsg) + 1; // Longueur du message d'erreur

    // Construction du paquet d'erreur
    memcpy(buffer, &opcode, 2);
    memcpy(buffer + 2, &errCode, 2);
    strcpy(buffer + 4, errorMsg);

    // Envoi du paquet d'erreur au client
    sendto(sock, buffer, 4 + msgLen, 0, (struct sockaddr *)client_addr, client_len);
    printf("[ERREUR] %s (Code %d)\n", errorMsg, errorCode);
}

// Traitement d'une requête RRQ (Read Request)
void handle_rrq(int sock, struct sockaddr_in *client_addr, socklen_t client_len, char *filename) {
    printf("[INFO] Requête RRQ reçue pour : %s\n", filename);

    FILE *file = fopen(filename, "rb");
    if (!file) {
        send_error(sock, client_addr, client_len, 1, "Fichier non trouvé"); // Erreur si le fichier n'existe pas
        return;
    }

    uint16_t blocknum = 1; // Numéro du premier bloc de données
    char buffer[516]; // Buffer pour l'envoi de données
    int read_size;
    
    do {
        read_size = fread(buffer + 4, 1, TFTP_BLOCK_SIZE, file); // Lecture d'un bloc du fichier
        
        uint16_t opcode = htons(OPCODE_DATA); // Opcode pour les données
        uint16_t net_blocknum = htons(blocknum); // Numéro de bloc en format réseau
        memcpy(buffer, &opcode, 2);
        memcpy(buffer + 2, &net_blocknum, 2);

        // Envoi du paquet DATA au client
        sendto(sock, buffer, read_size + 4, 0, (struct sockaddr *)client_addr, client_len);
        printf("[INFO] Envoyé DATA (Bloc %d, Taille : %d octets)\n", blocknum, read_size);

        // Attente de l'ACK du client
        char ack[4];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int retries = 0;

        // Tentatives de réémission en cas de perte de paquet
        while (retries < MAX_RETRIES) {
            int recv_len = recvfrom(sock, ack, sizeof(ack), 0, (struct sockaddr *)&from_addr, &from_len);
            if (recv_len >= 4 && ntohs(*(uint16_t *)ack) == OPCODE_ACK && ntohs(*(uint16_t *)(ack + 2)) == blocknum) {
                printf("[INFO] Reçu ACK pour le bloc %d\n", blocknum);
                break;
            }
            retries++;
            sendto(sock, buffer, read_size + 4, 0, (struct sockaddr *)client_addr, client_len);  // Réémission du bloc
            printf("[WARNING] Réémission du bloc %d\n", blocknum);
        }

        if (retries >= MAX_RETRIES) {
            printf("[ERREUR] Timeout dépassé pour le bloc %d\n", blocknum);
            break;
        }

        blocknum++; // Incrément du numéro du bloc
    } while (read_size == TFTP_BLOCK_SIZE); // Continue tant que le bloc est plein

    fclose(file);
    printf("[INFO] Fin du transfert du fichier %s\n", filename);
}

// Traitement d'une requête WRQ (Write Request)
void handle_wrq(int sock, struct sockaddr_in *client_addr, socklen_t client_len, char *filename) {
    printf("[INFO] Requête WRQ reçue pour : %s\n", filename);

    FILE *file = fopen(filename, "wb"); // Ouverture du fichier en mode écriture binaire
    if (!file) {
        send_error(sock, client_addr, client_len, 2, "Accès refusé"); // Erreur si l'accès est refusé
        return;
    }

    uint16_t blocknum = 0; // Numéro du bloc d'écriture
    char buffer[516];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    while (1) {
        uint16_t ack_opcode = htons(OPCODE_ACK); // Création de l'ACK
        uint16_t ack_blocknum = htons(blocknum);
        memcpy(buffer, &ack_opcode, 2);
        memcpy(buffer + 2, &ack_blocknum, 2);

        // Envoi de l'ACK pour le bloc actuel
        sendto(sock, buffer, 4, 0, (struct sockaddr *)client_addr, client_len);
        printf("[INFO] Envoyé ACK pour le bloc %d\n", blocknum);

        int recv_len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
        if (recv_len < 4) {
            send_error(sock, client_addr, client_len, 0, "Erreur de transmission");
            break;
        }

        uint16_t opcode, data_blocknum;
        memcpy(&opcode, buffer, 2);
        memcpy(&data_blocknum, buffer + 2, 2);
        opcode = ntohs(opcode);
        data_blocknum = ntohs(data_blocknum);

        if (opcode == OPCODE_ERROR) {
            printf("[ERREUR] Erreur TFTP reçue : %s\n", buffer + 4);
            break;
        }

        if (opcode == OPCODE_DATA && data_blocknum == blocknum + 1) {
            fwrite(buffer + 4, 1, recv_len - 4, file); // Écriture des données reçues dans le fichier
            printf("[INFO] Reçu DATA (Bloc %d, Taille : %d octets)\n", data_blocknum, recv_len - 4);
            blocknum++;

            if (recv_len < 516) {  // Si le bloc est plus petit que la taille maximale, le transfert est terminé
                // Envoyer l'ACK final
                uint16_t ack_opcode = htons(OPCODE_ACK);
                uint16_t ack_blocknum = htons(blocknum);
                memcpy(buffer, &ack_opcode, 2);
                memcpy(buffer + 2, &ack_blocknum, 2);
            
                sendto(sock, buffer, 4, 0, (struct sockaddr *)client_addr, client_len);
                printf("[INFO] Envoyé ACK final pour le bloc %d\n", blocknum);

                printf("[INFO] Fin du transfert, dernier bloc reçu : %d\n", blocknum);

                break;
            }
        }
    }
    

    fclose(file);
}

// Fonction principale du serveur TFTP
int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0); // Création d'un socket UDP
    if (sock < 0) {
        perror("Erreur de création du socket");
        return 1;
    }

    struct sockaddr_in server_addr, client_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TFTP_PORT);  // Utilisation du port TFTP standard
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Liaison du socket au port
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erreur de liaison du socket");
        return 1;
    }

    printf("[INFO] Serveur TFTP démarré sur le port %d...\n", TFTP_PORT);

    char buffer[516];
    socklen_t client_len = sizeof(client_addr);

    while (1) {
        // Attente de la réception d'une requête
        int recv_len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_len);
        if (recv_len < 4) continue; // Ignorer les paquets invalides

        uint16_t opcode = ntohs(*(uint16_t *)buffer); // Récupération de l'opcode de la requête
        char *filename = buffer + 2; // Le nom du fichier est après les 2 premiers octets (opcode)

        if (opcode == OPCODE_RRQ) { // Si c'est une requête de lecture
            handle_rrq(sock, &client_addr, client_len, filename);
        } else if (opcode == OPCODE_WRQ) { // Si c'est une requête d'écriture
            handle_wrq(sock, &client_addr, client_len, filename);
        } else { // Si l'opcode est invalide
            send_error(sock, &client_addr, client_len, 4, "Requête invalide");
        }
    }

    close(sock);
    return 0;
}
