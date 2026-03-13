#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>

#define BOARD_SIZE       9
#define SHM_KEY          0x1234
#define SEM_KEY          0x5678

#define SEM_JOGADA       0
#define SEM_MUTEX        1
#define SEM_VEZ_J1       2
#define SEM_VEZ_J2       3
#define SEM_INICIO       4
#define NUM_SEMS         5

#define RESULTADO_NENHUM 0
#define RESULTADO_J1     1
#define RESULTADO_J2     2
#define RESULTADO_EMPATE 3

typedef struct {
    char tabuleiro[BOARD_SIZE];
    int  jogada;
    int  resultado;
    int  jogo_ativo;
    int  j1_pronto;
    int  j2_pronto;
    int  turno;
} EstadoJogo;

static void sem_op_fn(int semid, int idx, int op) {
    struct sembuf sb = { (unsigned short)idx, (short)op, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop");
        exit(1);
    }
}
#define SEM_WAIT(id, idx) sem_op_fn((id), (idx), -1)
#define SEM_POST(id, idx) sem_op_fn((id), (idx),  1)

void imprimir_tabuleiro(const char *t) {
    printf("\n");
    for (int i = 0; i < 9; i += 3) {
        printf(" %c | %c | %c \n",
               t[i]   == ' ' ? '0'+i   : t[i],
               t[i+1] == ' ' ? '0'+i+1 : t[i+1],
               t[i+2] == ' ' ? '0'+i+2 : t[i+2]);
        if (i < 6) printf("---+---+---\n");
    }
    printf("\n");
}

int verificar_vitoria(const char *t, char s) {
    for (int r = 0; r < 9; r += 3)
        if (t[r]==s && t[r+1]==s && t[r+2]==s) return 1;
    for (int c = 0; c < 3; c++)
        if (t[c]==s && t[c+3]==s && t[c+6]==s) return 1;
    if (t[0]==s && t[4]==s && t[8]==s) return 1;
    if (t[2]==s && t[4]==s && t[6]==s) return 1;
    return 0;
}

int tabuleiro_cheio(const char *t) {
    for (int i = 0; i < BOARD_SIZE; i++)
        if (t[i] == ' ') return 0;
    return 1;
}

void modo_servidor() {
    int shm_id = shmget(SHM_KEY, sizeof(EstadoJogo), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_id == -1) {
        shm_id = shmget(SHM_KEY, sizeof(EstadoJogo), 0666);
        if (shm_id != -1) shmctl(shm_id, IPC_RMID, NULL);
        shm_id = shmget(SHM_KEY, sizeof(EstadoJogo), IPC_CREAT | IPC_EXCL | 0666);
        if (shm_id == -1) { perror("shmget"); exit(1); }
    }

    EstadoJogo *e = (EstadoJogo *)shmat(shm_id, NULL, 0);
    if (e == (void *)-1) { perror("shmat"); exit(1); }

    memset(e->tabuleiro, ' ', BOARD_SIZE);
    e->jogada     = -1;
    e->resultado  = RESULTADO_NENHUM;
    e->jogo_ativo = 1;
    e->j1_pronto  = 0;
    e->j2_pronto  = 0;
    e->turno      = 1;

    int semid = semget(SEM_KEY, NUM_SEMS, IPC_CREAT | IPC_EXCL | 0666);
    if (semid == -1) {
        semid = semget(SEM_KEY, NUM_SEMS, 0666);
        if (semid != -1) semctl(semid, 0, IPC_RMID);
        semid = semget(SEM_KEY, NUM_SEMS, IPC_CREAT | IPC_EXCL | 0666);
        if (semid == -1) { perror("semget"); exit(1); }
    }

    semctl(semid, SEM_JOGADA, SETVAL, 0);
    semctl(semid, SEM_MUTEX,  SETVAL, 1);
    semctl(semid, SEM_VEZ_J1, SETVAL, 0);
    semctl(semid, SEM_VEZ_J2, SETVAL, 0);
    semctl(semid, SEM_INICIO, SETVAL, 0);

    printf("SERVIDOR INICIADO\n");
    printf("Aguardando Jogador 1 e Jogador 2 conectarem...\n");
    fflush(stdout);

    while (!(e->j1_pronto && e->j2_pronto))
        usleep(100000);

    printf("Ambos conectados! Iniciando jogo.\n");
    imprimir_tabuleiro(e->tabuleiro);
    fflush(stdout);

    /* Libera os dois jogadores da barreira de inicio */
    SEM_POST(semid, SEM_INICIO);
    SEM_POST(semid, SEM_INICIO);

    /* Libera apenas o jogador 1 para jogar */
    SEM_POST(semid, SEM_VEZ_J1);

    while (e->jogo_ativo) {
        SEM_WAIT(semid, SEM_JOGADA);

        int  pos   = e->jogada;
        int  turno = e->turno;
        char simb  = (turno == 1) ? 'X' : 'O';

        e->tabuleiro[pos] = simb;
        e->jogada = -1;

        printf("\n[Jogador %d (%c)] jogou na posicao %d\n", turno, simb, pos);
        imprimir_tabuleiro(e->tabuleiro);
        fflush(stdout);

        if (verificar_vitoria(e->tabuleiro, simb)) {
            e->resultado  = (turno == 1) ? RESULTADO_J1 : RESULTADO_J2;
            e->jogo_ativo = 0;
        } else if (tabuleiro_cheio(e->tabuleiro)) {
            e->resultado  = RESULTADO_EMPATE;
            e->jogo_ativo = 0;
        }

        if (!e->jogo_ativo) {
            SEM_POST(semid, SEM_VEZ_J1);
            SEM_POST(semid, SEM_VEZ_J2);
            break;
        }

        e->turno = (turno == 1) ? 2 : 1;
        SEM_POST(semid, (e->turno == 1) ? SEM_VEZ_J1 : SEM_VEZ_J2);
    }

    printf("\n");
    switch (e->resultado) {
        case RESULTADO_J1:     printf("  Jogador 1 (X) VENCEU!\n"); break;
        case RESULTADO_J2:     printf("  Jogador 2 (O) VENCEU!\n"); break;
        case RESULTADO_EMPATE: printf("  EMPATE!\n");                break;
    }
    printf("\n");

    sleep(2);
    shmdt(e);
    shmctl(shm_id, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);
}

void modo_jogador(int num_jogador) {
    int shm_id = shmget(SHM_KEY, sizeof(EstadoJogo), 0666);
    if (shm_id == -1) {
        fprintf(stderr, "Erro: servidor nao encontrado. Inicie o servidor primeiro.\n");
        exit(1);
    }

    EstadoJogo *e = (EstadoJogo *)shmat(shm_id, NULL, 0);
    if (e == (void *)-1) { perror("shmat"); exit(1); }

    int semid = semget(SEM_KEY, NUM_SEMS, 0666);
    if (semid == -1) { perror("semget"); exit(1); }

    char simbolo      = (num_jogador == 1) ? 'X' : 'O';
    int sem_minha_vez = (num_jogador == 1) ? SEM_VEZ_J1 : SEM_VEZ_J2;

    SEM_WAIT(semid, SEM_MUTEX);
    if (num_jogador == 1) e->j1_pronto = 1;
    else                  e->j2_pronto = 1;
    SEM_POST(semid, SEM_MUTEX);

    printf("Jogador %d (%c) conectado\n", num_jogador, simbolo);
    printf("Aguardando o outro jogador...\n");
    fflush(stdout);

    /* Barreira: aguarda o servidor liberar o inicio */
    SEM_WAIT(semid, SEM_INICIO);

    printf("Jogo iniciado!\n");
    imprimir_tabuleiro(e->tabuleiro);
    fflush(stdout);

    while (1) {
        SEM_WAIT(semid, sem_minha_vez);

        if (!e->jogo_ativo) break;

        imprimir_tabuleiro(e->tabuleiro);
        printf("Sua vez! (%c) Digite a posicao (0-8): ", simbolo);
        fflush(stdout);

        int pos;
        while (1) {
            if (scanf("%d", &pos) != 1) {
                while (getchar() != '\n');
                printf("Entrada invalida. Digite um numero (0-8): ");
                fflush(stdout);
                continue;
            }
            if (pos < 0 || pos > 8) {
                printf("Posicao invalida. Digite entre 0 e 8: ");
                fflush(stdout);
                continue;
            }
            if (e->tabuleiro[pos] != ' ') {
                printf("Posicao ocupada. Escolha outra: ");
                fflush(stdout);
                continue;
            }
            break;
        }

        SEM_WAIT(semid, SEM_MUTEX);
        e->jogada = pos;
        SEM_POST(semid, SEM_MUTEX);

        SEM_POST(semid, SEM_JOGADA);

        if (e->jogo_ativo) {
            printf("Aguardando o outro jogador...\n");
            fflush(stdout);
        }
    }

    imprimir_tabuleiro(e->tabuleiro);
    printf("\n");
    switch (e->resultado) {
        case RESULTADO_J1:     printf("  Jogador 1 (X) VENCEU!\n"); break;
        case RESULTADO_J2:     printf("  Jogador 2 (O) VENCEU!\n"); break;
        case RESULTADO_EMPATE: printf("  EMPATE!\n");                break;
    }
    printf("\n");

    shmdt(e);
}

/*
COMO RODAR:

gcc -o jogo jogo_da_velha.c
./jogo servidor
./jogo jogador 1
./jogo jogador 2

CASO QUEIRA LIMPAR OS SEMÁFOROS E MEMÓRIA COMPARTILHADA APÓS O JOGO, RODE:
ipcrm -M 0x1234; ipcrm -S 0x5678
*/

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso:\n");
        printf("  Terminal 1 (servidor): %s servidor\n", argv[0]);
        printf("  Terminal 2 (jogador1): %s jogador 1\n", argv[0]);
        printf("  Terminal 3 (jogador2): %s jogador 2\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "servidor") == 0) {
        modo_servidor();
    } else if (strcmp(argv[1], "jogador") == 0 && argc == 3) {
        int num = atoi(argv[2]);
        if (num != 1 && num != 2) {
            fprintf(stderr, "Numero do jogador deve ser 1 ou 2.\n");
            return 1;
        }
        modo_jogador(num);
    } else {
        fprintf(stderr, "Argumento invalido.\n");
        return 1;
    }

    return 0;
}