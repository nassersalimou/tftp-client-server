# Client–serveur TFTP en C

Implémentation pédagogique d'un client et d'un serveur TFTP utilisant des sockets UDP.

## Fonctionnalités

- requêtes de lecture RRQ et d'écriture WRQ ;
- transfert par blocs de 512 octets ;
- accusés de réception ACK ;
- gestion des paquets d'erreur TFTP ;
- délais d'attente et retransmissions ;
- transferts en mode `octet`.

## Compilation

```bash
make
```

## Exécution

Le port TFTP standard `69` peut nécessiter des privilèges administrateur sous Linux. Dans deux terminaux distincts :

```bash
./serveur
./client 127.0.0.1 get examples/test.txt
./client 127.0.0.1 put fichier.txt
```

La syntaxe du client est `./client <serveur_ip> <get/put> <fichier>`.

## Contenu

- `client.c` : création des requêtes et gestion des transferts côté client ;
- `serveur.c` : traitement des requêtes et transferts côté serveur ;
- `examples/` : petit fichier de test.

## Avertissement

Ce projet est une implémentation universitaire destinée à l'apprentissage des sockets UDP et du protocole TFTP. Il ne doit pas être utilisé comme serveur exposé en production.
