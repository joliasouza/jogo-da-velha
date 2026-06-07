#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define BOARD_SIZE       9
#define SHM_KEY          0x1234

#define RESULTADO_NENHUM 0
#define RESULTADO_J1     1
#define RESULTADO_J2     2
#define RESULTADO_EMPATE 3

/*
 * A sincronizacao entre os processos e feita com mutex e variaveis de
 * condicao, ambos guardados na memoria compartilhada para que o servidor
 * e os dois jogadores possam usa-los juntos.
 */
 
typedef struct {
    pthread_mutex_t mutex;

    pthread_cond_t  cond_inicio;
    pthread_cond_t  cond_jogada;
    pthread_cond_t  cond_vez_j1;
    pthread_cond_t  cond_vez_j2;

    char tabuleiro[BOARD_SIZE];
    int  jogada;
    int  resultado;
    int  jogo_ativo;
    int  j1_pronto;
    int  j2_pronto;
    int  turno;

    int  flag_inicio;
    int  flag_jogada;
    int  flag_vez_j1;
    int  flag_vez_j2;
} EstadoJogo;


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

static void init_sync(EstadoJogo *e) {
    pthread_mutexattr_t mattr;
    pthread_condattr_t  cattr;

    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&e->mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&e->cond_inicio,  &cattr);
    pthread_cond_init(&e->cond_jogada,  &cattr);
    pthread_cond_init(&e->cond_vez_j1,  &cattr);
    pthread_cond_init(&e->cond_vez_j2,  &cattr);
    pthread_condattr_destroy(&cattr);
}

static void destroy_sync(EstadoJogo *e) {
    pthread_mutex_destroy(&e->mutex);
    pthread_cond_destroy(&e->cond_inicio);
    pthread_cond_destroy(&e->cond_jogada);
    pthread_cond_destroy(&e->cond_vez_j1);
    pthread_cond_destroy(&e->cond_vez_j2);
}

//  Modo servidor
void modo_servidor(void) {
    // Cria (ou recria) o segmento de memoria compartilhada
    int shm_id = shmget(SHM_KEY, sizeof(EstadoJogo), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_id == -1) {
        shm_id = shmget(SHM_KEY, sizeof(EstadoJogo), 0666);
        if (shm_id != -1) shmctl(shm_id, IPC_RMID, NULL);
        shm_id = shmget(SHM_KEY, sizeof(EstadoJogo), IPC_CREAT | IPC_EXCL | 0666);
        if (shm_id == -1) { perror("shmget"); exit(1); }
    }

    EstadoJogo *e = (EstadoJogo *)shmat(shm_id, NULL, 0);
    if (e == (void *)-1) { perror("shmat"); exit(1); }

    // Inicializa estado
    memset(e->tabuleiro, ' ', BOARD_SIZE);
    e->jogada      = -1;
    e->resultado   = RESULTADO_NENHUM;
    e->jogo_ativo  = 1;
    e->j1_pronto   = 0;
    e->j2_pronto   = 0;
    e->turno       = 1;
    e->flag_inicio = 0;
    e->flag_jogada = 0;
    e->flag_vez_j1 = 0;
    e->flag_vez_j2 = 0;

    init_sync(e);

    printf("SERVIDOR INICIADO\n");
    printf("Aguardando Jogador 1 e Jogador 2 conectarem...\n");
    fflush(stdout);

    // Aguarda ambos os jogadores sinalizarem que estao prontos
    pthread_mutex_lock(&e->mutex);
    while (!(e->j1_pronto && e->j2_pronto))
        pthread_cond_wait(&e->cond_inicio, &e->mutex);
    pthread_mutex_unlock(&e->mutex);

    printf("Ambos conectados! Iniciando jogo.\n");
    imprimir_tabuleiro(e->tabuleiro);
    fflush(stdout);

    // Libera a barreira de inicio para ambos os jogadores
    pthread_mutex_lock(&e->mutex);
    e->flag_inicio = 2;          // dois jogadores aguardam
    pthread_cond_broadcast(&e->cond_inicio);

    // Libera o jogador 1 para jogar
    e->flag_vez_j1 = 1;
    pthread_cond_signal(&e->cond_vez_j1);
    pthread_mutex_unlock(&e->mutex);

    while (e->jogo_ativo) {
        // Aguarda uma jogada
        pthread_mutex_lock(&e->mutex);
        while (!e->flag_jogada)
            pthread_cond_wait(&e->cond_jogada, &e->mutex);
        e->flag_jogada = 0;

        int  pos   = e->jogada;
        int  turno = e->turno;
        pthread_mutex_unlock(&e->mutex);

        char simb = (turno == 1) ? 'X' : 'O';

        pthread_mutex_lock(&e->mutex);
        e->tabuleiro[pos] = simb;
        e->jogada = -1;
        pthread_mutex_unlock(&e->mutex);

        printf("\n[Jogador %d (%c)] jogou na posicao %d\n", turno, simb, pos);
        imprimir_tabuleiro(e->tabuleiro);
        fflush(stdout);

        pthread_mutex_lock(&e->mutex);
        if (verificar_vitoria(e->tabuleiro, simb)) {
            e->resultado  = (turno == 1) ? RESULTADO_J1 : RESULTADO_J2;
            e->jogo_ativo = 0;
        } else if (tabuleiro_cheio(e->tabuleiro)) {
            e->resultado  = RESULTADO_EMPATE;
            e->jogo_ativo = 0;
        }

        if (!e->jogo_ativo) {
            // Acorda ambos para que leiam o resultado final
            e->flag_vez_j1 = 1;
            e->flag_vez_j2 = 1;
            pthread_cond_signal(&e->cond_vez_j1);
            pthread_cond_signal(&e->cond_vez_j2);
            pthread_mutex_unlock(&e->mutex);
            break;
        }

        // Passa a vez para o proximo jogador
        e->turno = (turno == 1) ? 2 : 1;
        if (e->turno == 1) {
            e->flag_vez_j1 = 1;
            pthread_cond_signal(&e->cond_vez_j1);
        } else {
            e->flag_vez_j2 = 1;
            pthread_cond_signal(&e->cond_vez_j2);
        }
        pthread_mutex_unlock(&e->mutex);
    }

    printf("\n");
    switch (e->resultado) {
        case RESULTADO_J1:     printf("  Jogador 1 (X) VENCEU!\n"); break;
        case RESULTADO_J2:     printf("  Jogador 2 (O) VENCEU!\n"); break;
        case RESULTADO_EMPATE: printf("  EMPATE!\n");                break;
    }
    printf("\n");

    sleep(2);
    destroy_sync(e);
    shmdt(e);
    shmctl(shm_id, IPC_RMID, NULL);
}

//  Modo jogador
void modo_jogador(int num_jogador) {
    int shm_id = shmget(SHM_KEY, sizeof(EstadoJogo), 0666);
    if (shm_id == -1) {
        fprintf(stderr, "Erro: servidor nao encontrado. Inicie o servidor primeiro.\n");
        exit(1);
    }

    EstadoJogo *e = (EstadoJogo *)shmat(shm_id, NULL, 0);
    if (e == (void *)-1) { perror("shmat"); exit(1); }

    char simbolo = (num_jogador == 1) ? 'X' : 'O';

    // Sinaliza ao servidor que este jogador esta pronto
    pthread_mutex_lock(&e->mutex);
    if (num_jogador == 1) e->j1_pronto = 1;
    else                  e->j2_pronto = 1;
    pthread_cond_signal(&e->cond_inicio);   // notifica o servidor
    pthread_mutex_unlock(&e->mutex);

    printf("Jogador %d (%c) conectado\n", num_jogador, simbolo);
    printf("Aguardando o outro jogador...\n");
    fflush(stdout);

    // Barreira: aguarda o servidor sinalizar o inicio
    pthread_mutex_lock(&e->mutex);
    while (e->flag_inicio == 0)
        pthread_cond_wait(&e->cond_inicio, &e->mutex);
    e->flag_inicio--;   // consome um "ticket" de inicio
    pthread_mutex_unlock(&e->mutex);

    printf("Jogo iniciado!\n");
    imprimir_tabuleiro(e->tabuleiro);
    fflush(stdout);

    pthread_cond_t *minha_cond = (num_jogador == 1)
                                 ? &e->cond_vez_j1
                                 : &e->cond_vez_j2;
    int *minha_flag = (num_jogador == 1)
                      ? &e->flag_vez_j1
                      : &e->flag_vez_j2;

    while (1) {
        pthread_mutex_lock(&e->mutex);
        while (!(*minha_flag))
            pthread_cond_wait(minha_cond, &e->mutex);
        (*minha_flag) = 0;
        pthread_mutex_unlock(&e->mutex);

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
            pthread_mutex_lock(&e->mutex);
            int ocupada = (e->tabuleiro[pos] != ' ');
            pthread_mutex_unlock(&e->mutex);
            if (ocupada) {
                printf("Posicao ocupada. Escolha outra: ");
                fflush(stdout);
                continue;
            }
            break;
        }

        pthread_mutex_lock(&e->mutex);
        e->jogada      = pos;
        e->flag_jogada = 1;
        pthread_cond_signal(&e->cond_jogada);
        pthread_mutex_unlock(&e->mutex);

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
COMO COMPILAR E RODAR:

  gcc -o jogo jogo_da_velha.c -lpthread

  Terminal 1 (servidor): ./jogo servidor
  Terminal 2 (jogador1): ./jogo jogador 1
  Terminal 3 (jogador2): ./jogo jogador 2

CASO PRECISE LIMPAR A MEMORIA COMPARTILHADA MANUALMENTE:
  ipcrm -M 0x1234
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