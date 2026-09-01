#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

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

// Structure pour la requête TFTP (RRQ ou WRQ)
typedef struct {
    uint16_t opcode;
    char filename[256];
    char mode[10]; // "octet" ou "netascii"
} TFTP_Request;

// Structure pour le paquet de données TFTP
typedef struct {
    uint16_t opcode;
    uint16_t blocknum;
    char data[TFTP_BLOCK_SIZE];
} TFTP_DataPacket;

// Structure pour l'accusé de réception (ACK) TFTP
typedef struct {
    uint16_t opcode;
    uint16_t blocknum;
} TFTP_AckPacket;

// Structure pour un message d'erreur TFTP
typedef struct {
    uint16_t opcode;
    uint16_t errorCode;
    char errorMsg[256];
} TFTP_ErrorPacket;

// Fonction pour créer un socket UDP
int create_socket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Erreur de création de socket");
        exit(1);
    }

    // Configuration du timeout de réception
    struct timeval timeout = {TIMEOUT, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    return sock;
}

// Fonction pour envoyer une requête (RRQ ou WRQ) au serveur
void send_request(int sock, struct sockaddr_in *server_addr, const char *filename, uint16_t opcode) {
    char buffer[516];
    int len = sprintf(buffer, "%c%c%s%c%s%c", 0, opcode, filename, 0, "octet", 0);
    
    // Envoi de la requête au serveur via le socket
    if (sendto(sock, buffer, len, 0, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0) {
        perror("Erreur lors de l'envoi de la requête");
        exit(1);
    }
}

// Fonction pour traiter un message d'erreur TFTP
void handle_tftp_error(char *buffer) {
    uint16_t errorCode;
    memcpy(&errorCode, buffer + 2, 2);
    errorCode = ntohs(errorCode);
    printf("Erreur TFTP : Code %d - %s\n", errorCode, buffer + 4);
}

// Fonction pour télécharger un fichier depuis le serveur TFTP
void download_file(int sock, struct sockaddr_in *server_addr, const char *filename) {
    send_request(sock, server_addr, filename, OPCODE_RRQ);
    printf("[INFO] Demande de téléchargement envoyée pour le fichier : %s\n", filename);

    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Erreur ouverture fichier");
        return;
    }

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    char buffer[516];
    uint16_t expected_block = 1;
    int retries;

    // Boucle principale pour recevoir les paquets DATA
    while (1) {
        retries = 0;
        int recv_len;

        // Tentatives de réception des paquets
        do {
            recv_len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
            if (recv_len < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    retries++;
                    if (retries >= MAX_RETRIES) {
                        printf("[ERREUR] Timeout dépassé pour le bloc %d. Arrêt du transfert.\n", expected_block);
                        fclose(file);
                        return; // Si le timeout est atteint, arrêter le transfert
                    }
                    printf("[WARNING] Timeout, retransmission de l'ACK du bloc %d...\n", expected_block - 1);
                    // Retransmettre le dernier ACK
                    char ack[4];
                    uint16_t ack_opcode = htons(OPCODE_ACK);
                    uint16_t ack_blocknum = htons(expected_block - 1);
                    memcpy(ack, &ack_opcode, 2);
                    memcpy(ack + 2, &ack_blocknum, 2);
                    sendto(sock, ack, sizeof(ack), 0, (struct sockaddr *)&from_addr, from_len);
                } else {
                    perror("Erreur réception");
                    fclose(file);
                    return;
                }
            }
        } while (recv_len < 0);

        uint16_t opcode, blocknum;
        memcpy(&opcode, buffer, 2); // Extraire le code d'opération
        memcpy(&blocknum, buffer + 2, 2); // Extraire le numéro de bloc
        opcode = ntohs(opcode);
        blocknum = ntohs(blocknum);

        // Vérifier les erreurs reçues
        if (opcode == OPCODE_ERROR) {
            handle_tftp_error(buffer);
            fclose(file);
            return;
        }

        // Vérifier si le paquet DATA est valide
        if (opcode == OPCODE_DATA && blocknum == expected_block) {
            printf("[INFO] Reçu DATA (Bloc %d, Taille : %d octets)\n", blocknum, recv_len - 4);
            fwrite(buffer + 4, 1, recv_len - 4, file);
            expected_block++;

            // Envoi d’un ACK
            char ack[4];
            uint16_t ack_opcode = htons(OPCODE_ACK);
            uint16_t ack_blocknum = htons(blocknum);
            memcpy(ack, &ack_opcode, 2);
            memcpy(ack + 2, &ack_blocknum, 2);
            sendto(sock, ack, sizeof(ack), 0, (struct sockaddr *)&from_addr, from_len);
            printf("[INFO] Envoyé ACK pour le bloc %d\n", blocknum);
        }

        // Si la taille du paquet est inférieure à 516, la fin du transfert est atteinte
        if (recv_len < 516) {
            printf("[INFO] Fin du transfert, dernier bloc reçu : %d\n", blocknum);
            break;
        }
    }

    fclose(file);
}

// Fonction pour télécharger un fichier sur le serveur TFTP
void upload_file(int sock, struct sockaddr_in *server_addr, const char *filename) {
    send_request(sock, server_addr, filename, OPCODE_WRQ); // Envoyer la requête d'écriture (WRQ)
    printf("[INFO] Demande d’envoi envoyée pour le fichier : %s\n", filename);

    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Erreur ouverture fichier");
        return;
    }

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    char buffer[516]; // Tampon pour les paquets
    uint16_t blocknum = 0;
    int retries;

    // Boucle principale pour envoyer les paquets de données
    while (1) {
        retries = 0;
        int recv_len;

        // Tentatives de réception des ACK
        do {
            recv_len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
            if (recv_len < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    retries++;
                    if (retries >= MAX_RETRIES) {
                        printf("[ERREUR] Timeout dépassé pour le bloc %d. Arrêt du transfert.\n", blocknum);
                        fclose(file);
                        return;
                    }
                    printf("[WARNING] Timeout, retransmission du dernier bloc %d...\n", blocknum);
                    sendto(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, from_len);
                } else {
                    perror("Erreur réception");
                    fclose(file);
                    return;
                }
            }
        } while (recv_len < 0);

        uint16_t opcode, ack_blocknum;
        memcpy(&opcode, buffer, 2);
        memcpy(&ack_blocknum, buffer + 2, 2);
        opcode = ntohs(opcode);
        ack_blocknum = ntohs(ack_blocknum);

        if (opcode == OPCODE_ERROR) {
            handle_tftp_error(buffer);
            fclose(file);
            return;
        }

        // Vérifier l'ACK du bloc reçu
        if (opcode == OPCODE_ACK && ack_blocknum == blocknum) {
            printf("[INFO] Reçu ACK pour le bloc %d\n", blocknum);
            blocknum++;
            int read_size = fread(buffer + 4, 1, TFTP_BLOCK_SIZE, file); // Lire les données du fichier
            if (read_size == 0) {
                printf("[INFO] Fin du transfert, dernier bloc envoyé : %d\n", blocknum - 1);
                break;
            }

            uint16_t data_opcode = htons(OPCODE_DATA);
            uint16_t data_blocknum = htons(blocknum);
            memcpy(buffer, &data_opcode, 2);
            memcpy(buffer + 2, &data_blocknum, 2);

            sendto(sock, buffer, read_size + 4, 0, (struct sockaddr *)&from_addr, from_len); // Envoyer les données
            printf("[INFO] Envoyé DATA (Bloc %d, Taille : %d octets)\n", blocknum, read_size);
        }
    }

    fclose(file);
}

// Fonction principale
int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Utilisation : %s <serveur_ip> <get/put> <fichier>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1]; // Adresse IP du serveur
    const char *operation = argv[2]; // Opération (get ou put)
    const char *filename = argv[3]; // Nom du fichier à transférer

    int sock = create_socket(); // Créer le socket UDP
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TFTP_PORT);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr); // Convertir l'adresse IP en format binaire

    if (strcmp(operation, "get") == 0) {
        download_file(sock, &server_addr, filename);
    } else if (strcmp(operation, "put") == 0) {
        upload_file(sock, &server_addr, filename);
    } else {
        printf("Opération invalide !\n");
    }

    close(sock);
    return 0;
}
