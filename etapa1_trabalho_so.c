#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

//----------------------------------------------------------------------------------
// Constantes - Controle das principais funções do algoritmo pode ser feito por elas
const int SLEEP_TIME = 1000000;             // microssegundos   - 1000000
const double REGULAR_WORKING_TIME = 80;     // segundos         - 80
const double EXTRA_WORKING_TIME = 10;       // segundos         - 10
const int HELMET_AMOUNT = 10;               //                  - 10
const int KART_AMOUNT = 10;                 //                  - 10        
const int MIN_RACING_TIME = 1000000;        // microssegundos   - 1000000
const int MAX_RACING_TIME = 5000000;        // microssegundos   - 5000000
const int MAX_THREADS = 500;                //                  - 500
//----------------------------------------------------------------------------------

// Mutex e Semáforos
sem_t helmets;
sem_t karts;
pthread_mutex_t sem_access;                 // Controla o acesso aos recursos
pthread_mutex_t clients_served_access;      // Controla o acesso a global "clients_served_total"

// Enums e Structs
enum CLIENT_TYPE {
    ADULT,
    KID,
    KID14
};

struct client {
    int id;
    pthread_t thread;
    enum CLIENT_TYPE type;
    time_t time_arrived;
    double time_waited;
    int clock_ticks;                        // Quantidade de ciclos de repetição esperados
    unsigned int seed;                      // Seed necessária para algoritmo aleatório "thread-safe"
    int priority;
    struct client *next;
};

struct client_list_head {
    int size;
    struct client *next;
};

// Globais
int is_closing_time = 0;                    // Controla quando as threads devem abortar (tempo extra acabou)
int clients_served_total = 0;               // Total de clientes que conseguiram correr de kart, tem seu
                                            // valor atualizado em tempo real conforme as threads retornam

// Funções
void* go_race(void *arg);
char* get_client_type_string(enum CLIENT_TYPE type);
int get_random_int(int min, int max);
int get_thread_random_int(unsigned int *seed, int min, int max);
double get_elapsed_time(time_t start_time);
double get_elapsed_exec_time(time_t start_time);
struct client_list_head* create_list_head ();
void add_client_to_list(struct client *new_client, struct client_list_head *list_head);
struct client* remove_first_client_from_list(struct client_list_head *list_head);
int free_list(struct client_list_head *list_head);

// Funções de Debug
void debug_print_list();

int main() {
    /*
    Obs: Tempo extra Se refere ao período após o fechamento em que clientes que esperavam por sua vez no
    kartódromo ainda são atendidos. Novos clientes não são admitidos após o horário normal de fechamento.
    Obs 2: Linux desconsidera o tempo passado durante a execução de "sleep()" no retorno da função "clock()"
    */

    sem_init(&helmets, 0, HELMET_AMOUNT);
    sem_init(&karts, 0, KART_AMOUNT);
    pthread_mutex_init(&sem_access, NULL);
    pthread_mutex_init(&clients_served_access, NULL);

    srand(time(NULL));

    struct client_list_head *client_list_head = create_list_head();
    struct client_list_head *client_executed_list_head = create_list_head();
    
    int clients_total = 0;
    int id = 0;
    int adult_counter = 0;
    int kid_counter = 0;
    int kid14_counter = 0;

    time_t exec_time_start = clock();       // Variáveis responsáveis por controlar os ciclos principais de
    double exec_time = 0.0;                 // tempo de trabalho padrão e extra

    while (exec_time < REGULAR_WORKING_TIME) {
        struct client* client = NULL;
        printf("-- Tempo = %.2f/%.2f --\n", exec_time, REGULAR_WORKING_TIME);

        int client_arrived_chance = get_random_int(0, 100);
        int client_group_size = 0;

        /*
        35% de chance de não entrar ninguém
        60% de chance de entrar de 1 a 4 pessoas
        5% de chance de entrar de 3 a 10 pessoas
        */
       
        if (client_arrived_chance < 35) {
            client_group_size = 0;
            printf("Sem novos clientes\n");
        } else 
        if (client_arrived_chance < 95) {
            client_group_size = get_random_int(1, 4);
        } else {
            client_group_size = get_random_int(5, 10);
        }
        
        // Checa se o limite de threads foi atingido
        if (clients_total + client_group_size > MAX_THREADS) {
            if (clients_total < MAX_THREADS) {
                client_group_size = MAX_THREADS - clients_total;
            } else {
                for (
                    client = client_list_head->next;
                    client != NULL;
                    client = client->next
                ) {
                    client->priority++;
                }

                usleep(SLEEP_TIME);
                exec_time += get_elapsed_exec_time(exec_time_start);
                continue;
            }
        }
        clients_total += client_group_size;

        for (int i = 0; i < client_group_size; i++) {
            int client_type = get_random_int(0, 2);
            
            // Decide se o cliente é um adulto, criança ou criança menor de 14
            struct client *new_client = (struct client *) malloc(sizeof(struct client));
            new_client->id = id++;
            switch (client_type) {
                case 0:
                    new_client->type = ADULT;
                    new_client->priority = 0;
                    adult_counter++;
                    break;
                case 1:
                    new_client->type = KID;
                    new_client->priority = 5;
                    kid_counter++;
                    break;
                case 2:
                    new_client->type = KID14;
                    new_client->priority = 10;
                    kid14_counter++;
                    break;
            }
            new_client->time_waited = 0;
            new_client->clock_ticks = 0;
            // Seed para a função rand_r(), funciona como o srand() para a função rand()
            new_client->seed = (unsigned int) ( (unsigned int) time(NULL) ) + ( (unsigned int) get_random_int(0, 1000) );
            new_client->next = NULL;

            add_client_to_list(new_client, client_list_head);
            new_client->time_arrived = clock();
            printf(
                "Cliente %u - p%d (%s) chegou\n",
                new_client->id, new_client->priority, get_client_type_string(new_client->type)
            );
        }

        /*
        "helmets_available" e "karts_available" podem pegar o valor dos semáforos desatualizados. Pode ser
        interpretado como um cliente vendo que há capacetes e karts disponíveis, mas alguém "pula na
        frente". De qualquer modo, a thread vai esperar pela liberação dos recursos na execução de
        "go_race".
        */

        int helmets_available;
        sem_getvalue(&helmets, &helmets_available);

        int karts_available;
        sem_getvalue(&karts, &karts_available);

        /*
        De acordo com o número de clientes, karts e capacetes disponíveis, calcula a quantidade de threads
        que podem ser executados nesta repetição
        */
        int active_size = 0;
        for (
            client = client_list_head->next;
            (helmets_available > 0 && karts_available > 0) && client != NULL;
            client = client->next, helmets_available--, karts_available--   
        ) {
            active_size++;
        }

        // Aumenta a prioridade dos que ainda não executaram
        if (client != NULL) {
            for (
                ;
                client != NULL;
                client = client->next
            ) {
                client->clock_ticks++;
                client->priority++;
            }
        }

        /*
        Executa as threads, remove suas structs da lista de espera e as coloca na lista de execução (threads que
        estão no processo de execução ou já o finalizaram).
        */
        while(active_size > 0) {
            struct client *first_client = remove_first_client_from_list(client_list_head);
            pthread_create(&first_client->thread, NULL, go_race, first_client);
            add_client_to_list(first_client, client_executed_list_head);
            active_size--;
        }

        // Espera para o próximo ciclo de repetição
        usleep(SLEEP_TIME);
        // Atualiza o tempo de execução
        exec_time += get_elapsed_exec_time(exec_time_start);
    }
    // Tempo de fim do horário normal
    double closing_time = exec_time;

    pthread_mutex_lock(&clients_served_access);
    // Clientes servidos durante o horário normal
    int clients_served_regular_time = clients_served_total;
    pthread_mutex_unlock(&clients_served_access);

    printf("Horário de trabalho acabou!\n");

    // Horário extra
    while (exec_time < REGULAR_WORKING_TIME + EXTRA_WORKING_TIME) {
        printf("-- Tempo = %.2f/%.2f --\n", exec_time - closing_time, EXTRA_WORKING_TIME);
        /*
        "helmets_available" e "karts_available" podem pegar o valor dos semáforos desatualizados. Pode ser
        interpretado como um cliente vendo que há capacetes e karts disponíveis, mas alguém "pula na
        frente". De qualquer modo, a thread vai esperar pela liberação dos recursos na execução de
        "go_race".
        */

        int helmets_available;
        sem_getvalue(&helmets, &helmets_available);

        int karts_available;
        sem_getvalue(&karts, &karts_available);

        /*
        De acordo com o número de clientes, karts e capacetes disponíveis, calcula a quantidade de threads
        que podem ser executados nesta repetição
        */
        struct client *client;

        int active_size = 0;
        for (
            client = client_list_head->next;
            (helmets_available > 0 && karts_available > 0) && client != NULL;
            client = client->next, helmets_available--, karts_available--   
        ) {
            active_size++;
        }

        // Aumenta a prioridade dos que ainda não executaram
        if (client != NULL) {
            for (
                ;
                client != NULL;
                client = client->next
            ) {
                client->clock_ticks++;
                client->priority++;
            }
        }

        /*
        Executa as threads, remove suas structs da lista de espera e as coloca na lista de execução (threads que
        estão no processo de execução ou já o finalizaram).
        */
        while(active_size > 0) {
            struct client *first_client = remove_first_client_from_list(client_list_head);
            pthread_create(&first_client->thread, NULL, go_race, first_client);
            add_client_to_list(first_client, client_executed_list_head);
            active_size--;
        }
        usleep(SLEEP_TIME);
        // Atualiza o tempo de execução
        exec_time += get_elapsed_exec_time(exec_time_start);
    }
    // Da a ordem para que threads que ainda não iniciaram o processor de "corrida" se encerrem
    is_closing_time = 1;

    // Tempo de fim do horário extra
    double extra_time = exec_time - closing_time;

    pthread_mutex_lock(&clients_served_access);
    int clients_served_extra_time = clients_served_total - clients_served_regular_time;
    pthread_mutex_unlock(&clients_served_access);

    printf("Tempo extra acabou!\n");

    // Calcula o tempo médio de espera dos clientes (lista de execução e de espera)
    // Clock ticks calcula os ciclos de SLEEP_TIME desconsiderados no retorno da função clock()
    double average_waiting_time = 0.0;
    for (struct client *client = client_list_head->next; client != NULL; client = client->next) {
        client->time_waited = get_elapsed_time(client->time_arrived) +
            (double) ( (double) client->clock_ticks * (double) (SLEEP_TIME / 1000000.0) );
        average_waiting_time += client->time_waited;
    }
    for (struct client *client = client_executed_list_head->next; client != NULL; client = client->next) {
        client->time_waited += (double) ( (double) client->clock_ticks * (double) (SLEEP_TIME / 1000000.0) );
        average_waiting_time += client->time_waited;
        pthread_join(client->thread, NULL);
    }
    average_waiting_time = average_waiting_time / (double) clients_total;

    printf("\n--//--\n");
    printf("Clientes presentes no dia = %d\n", clients_total);
    printf("Clientes adultos = %d (%.2f%%)\n", adult_counter, adult_counter * 100.0 / clients_total);
    printf("Clientes crianças = %d (%.2f%%)\n", kid_counter, kid_counter * 100.0 / clients_total);
    printf("Clientes crianças menores de 14 anos = %d (%.2f%%)\n", kid14_counter, kid14_counter * 100.0 / clients_total);
    printf("Total de clientes atendidos = %d\n", clients_served_total);
    printf("Clientes atendidos no tempo extra = %d\n", clients_served_extra_time);
    printf("Clientes não atendidos = %d\n", clients_total - clients_served_total);
    printf("Tempo de trabalho = %.2f segundos\n", closing_time);
    printf("Tempo de trabalho extra = %.2f segundos\n", extra_time);
    printf("Tempo total = %.2f segundos\n", closing_time + extra_time);
    printf("Tempo médio de espera = %.2f segundos\n", average_waiting_time);

    // Libera a memória
    free_list(client_list_head);
    free_list(client_executed_list_head);

    sem_destroy(&helmets);
    sem_destroy(&karts);
    pthread_mutex_destroy(&sem_access);
    pthread_mutex_destroy(&clients_served_access);
    printf("\nSimulação encerrada\n");
}

// Função executada pelas threads
void* go_race(void *arg) {
    struct client *client = (struct client *) arg;

    // Mutex "sem_access" evita deadlock
    pthread_mutex_lock(&sem_access);
    if (client->type == ADULT) {
        sem_wait(&karts);
        sem_wait(&helmets);
    } else {
        sem_wait(&helmets);
        sem_wait(&karts);
    }
    pthread_mutex_unlock(&sem_access);

    // Se o kartódromo já tiver fechado, não executa
    if (is_closing_time) {
        client->time_waited = get_elapsed_time(client->time_arrived);
        printf(
            "Cliente %u - p%d (%s) não poderá correr - "
            "Tempo de espera = %.2f\n", 
            client->id, client->priority, get_client_type_string(client->type), client->time_waited
        );
    } else {
        printf("Cliente %u - p%d (%s) está correndo!\n", client->id, client->priority, get_client_type_string(client->type));

        // O tempo de espera é aleatorizado entre o tempo máximo e mínimo
        int racing_time = get_thread_random_int(&(client->seed), MIN_RACING_TIME, MAX_RACING_TIME);
        usleep(racing_time);

        client->time_waited = get_elapsed_time(client->time_arrived);

        pthread_mutex_lock(&clients_served_access);
        clients_served_total++;
        pthread_mutex_unlock(&clients_served_access);
        
        printf(
            "Cliente %u - p%d (%s) terminou - "
            "Tempo total = %.2f\n", 
            client->id, client->priority, get_client_type_string(client->type), client->time_waited
        );
    }

    sem_post(&karts);
    sem_post(&helmets);
}

// Retorna em texto o enum do cliente
char* get_client_type_string(enum CLIENT_TYPE type) {
    switch (type) {
        case ADULT:
            return "Adulto";
            break;
        case KID:
            return "Criança";
            break;
        case KID14:
            return "Criança menor de 14";
            break;
    }
    return "???";
}

// Aleatoriza um valor entre min e max
int get_random_int(int min, int max) {
    return ( ( rand() % ( max - min + 1 ) ) + min );
}

// Aleatoriza um valor entre min e max (thread-safe)
int get_thread_random_int(unsigned int *seed, int min, int max) {
    return ( ( rand_r(seed) % ( max - min + 1 ) ) + min );
}

// Retorna o tempo que se passou em segundos entre "start_time" e o momento atual
double get_elapsed_time(time_t start_time) {
    return (double) ( (double) clock() - (double) start_time ) / (double) CLOCKS_PER_SEC;
}

// Retorna o tempo que se passou em segundos entre "start_time" e o momento atual
// Obs: Variação da função acima que considera o tempo de "sleep()" na função "clock()". Utilizado para
// conseguir o valor correto da variável "exec_time"
double get_elapsed_exec_time(time_t start_time) {
    return (double) ( (double) ( (double) clock() - (double) start_time ) / (double) CLOCKS_PER_SEC ) +
        ( (double) SLEEP_TIME / 1000000.0 );
}

// Retorna uma cabeça de lista encadeada
struct client_list_head* create_list_head () {
    struct client_list_head *list_head = (struct client_list_head*) malloc(sizeof(struct client_list_head));
    list_head->size = 0;
    list_head->next = NULL;

    return list_head;
}

// Adiciona o cliente passado a lista encadeada passada, em ordem de prioridade
void add_client_to_list(struct client *new_client, struct client_list_head *list_head) {
    struct client *client_prev = NULL;

    for (
        struct client *client = list_head->next;
        client != NULL && new_client->priority < client->priority;
        client = client->next
    ) {
        client_prev = client;
    }

    if (client_prev == NULL) {
        new_client->next = list_head->next;
        list_head->next = new_client;
    } else {
        new_client->next = client_prev->next;
        client_prev->next = new_client;
    }

    list_head->size++;
}

// Remove o primeiro cliente da lista encadeada passada como parâmetro e o retorna
struct client* remove_first_client_from_list(struct client_list_head *list_head) {
    struct client *first_client = list_head->next;
    list_head->next = first_client->next;

    list_head->size--;
    return first_client;
}

// Libera a memória da lista encadeada passada como parâmetro
int free_list(struct client_list_head *list_head) {
    int counter = 0;

    struct client *temp = NULL;
    for (struct client *client = list_head->next; client != NULL; client = temp) {
        temp = client->next;
        free(client);
        counter++;
    }
    free(list_head);

    return counter;
}

// Debug

// Printa o conteúdo de uma lista
void debug_print_list(struct client_list_head *list_head) {
    printf("\n");
    for (struct client *client = list_head->next; client != NULL; client = client->next) {
        printf("Cliente %-3d - p%-2d (%s) - %fs\n", client->id, client->priority, get_client_type_string(client->type), (double) client->time_arrived);
    }
    printf("\n");
}