#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libwebsockets.h>
#include <jansson.h>

#define MAX_PROCESSES 20
#define MAX_RESOURCES 10
#define MAX_STEPS 100
#define BUFFER_SIZE 4096
#define WS_PORT 8080

// 同步问题类型
typedef enum {
    PROBLEM_PRODUCER_CONSUMER,   // 生产者-消费者问题
    PROBLEM_READERS_WRITERS,     // 读者-写者问题
    PROBLEM_DINING_PHILOSOPHERS, // 哲学家就餐问题
    PROBLEM_SLEEPING_BARBER,     // 睡眠理发师问题
    PROBLEM_SMOKERS               // 吸烟者问题
} ProblemType;

// 进程状态枚举
typedef enum {
    PROCESS_RUNNING,   // 运行中
    PROCESS_WAITING,   // 等待资源
    PROCESS_BLOCKED,   // 阻塞
    PROCESS_TERMINATED // 已终止
} ProcessState;

// 进程角色枚举
typedef enum {
    ROLE_PRODUCER,      // 生产者
    ROLE_CONSUMER,      // 消费者
    ROLE_READER,        // 读者
    ROLE_WRITER,        // 写者
    ROLE_PHILOSOPHER,   // 哲学家
    ROLE_BARBER,        // 理发师
    ROLE_CUSTOMER,      // 顾客
    ROLE_SMOKER,        // 吸烟者
    ROLE_AGENT          // 供应者
} ProcessRole;

// 资源类型枚举
typedef enum {
    RESOURCE_BUFFER,    // 缓冲区
    RESOURCE_DATABASE,  // 数据库
    RESOURCE_FORK,      // 叉子
    RESOURCE_CHAIR,     // 椅子
    RESOURCE_TOBACCO,   // 烟草
    RESOURCE_PAPER,     // 纸
    RESOURCE_MATCH      // 火柴
} ResourceType;

// 资源结构体
typedef struct {
    int id;              // 资源ID
    char name[50];       // 资源名称
    ResourceType type;   // 资源类型
    int capacity;        // 资源容量
    int used;            // 已使用数量
    int *allocation;     // 各进程分配情况
    int owner_process;   // 当前拥有者进程ID（如为互斥资源）
    int is_mutex;        // 是否为互斥资源
} Resource;

// 进程结构体
typedef struct {
    int id;              // 进程ID
    char name[50];       // 进程名称
    ProcessRole role;    // 进程角色
    ProcessState state;  // 进程状态
    int resources[MAX_RESOURCES]; // 持有的资源
    int resource_count;  // 持有的资源数量
    int waiting_resource; // 等待的资源
    int steps[MAX_STEPS][3]; // 执行步骤 [步骤类型, 资源ID, 数量]
    int step_count;      // 步骤数量
    int current_step;    // 当前步骤
    char message[256];   // 进程消息
} Process;

// 模拟步骤类型
typedef enum {
    STEP_ACQUIRE,        // 获取资源
    STEP_RELEASE,        // 释放资源
    STEP_COMPUTE,        // 计算/处理
    STEP_WAIT,           // 等待
    STEP_MESSAGE         // 显示消息
} StepType;

// 同步问题结构体
typedef struct {
    ProblemType type;              // 问题类型
    char name[50];                 // 问题名称
    char description[256];         // 问题描述
    Resource resources[MAX_RESOURCES]; // 资源列表
    int resource_count;            // 资源数量
    Process processes[MAX_PROCESSES]; // 进程列表
    int process_count;             // 进程数量
    int simulation_step;           // 当前模拟步骤
    int is_running;                // 是否正在运行
    int speed;                     // 模拟速度(毫秒)
    sem_t *semaphores;             // 信号量数组
    pthread_mutex_t *mutexes;      // 互斥锁数组
} SynchronizationProblem;

// 全局变量
SynchronizationProblem current_problem;
int should_exit = 0;
struct lws_context *ws_context = NULL;
struct lws **clients = NULL;
int client_count = 0;
pthread_mutex_t sim_mutex = PTHREAD_MUTEX_INITIALIZER;

// 函数声明
void init_problem(ProblemType type);
void setup_producer_consumer();
void setup_readers_writers();
void setup_dining_philosophers();
void setup_sleeping_barber();
void setup_smokers();
void reset_simulation();
void simulate_step();
void run_simulation();
char* create_problem_json();
void broadcast_state();
void handle_client_message(const char* message);
void* simulation_thread(void* arg);

// WebSocket回调函数
int callback_sync_simulator(struct lws *wsi, enum lws_callback_reasons reason, 
                          void *user, void *in, size_t len);

// WebSocket协议定义
static struct lws_protocols protocols[] = {
    {
        "sync-simulator-protocol",
        callback_sync_simulator,
        0,
        BUFFER_SIZE,
    },
    { NULL, NULL, 0, 0 }
};

// WebSocket回调函数实现
int callback_sync_simulator(struct lws *wsi, enum lws_callback_reasons reason,
                          void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            printf("新的WebSocket连接已建立\n");
            pthread_mutex_lock(&sim_mutex);
            clients = realloc(clients, (client_count + 1) * sizeof(struct lws *));
            clients[client_count++] = wsi;
            pthread_mutex_unlock(&sim_mutex);
            broadcast_state();
            break;

        case LWS_CALLBACK_RECEIVE:
            if (in && len > 0) {
                char *message = malloc(len + 1);
                memcpy(message, in, len);
                message[len] = '\0';
                handle_client_message(message);
                free(message);
            }
            break;

        case LWS_CALLBACK_CLOSED:
            printf("WebSocket连接已关闭\n");
            pthread_mutex_lock(&sim_mutex);
            for (int i = 0; i < client_count; i++) {
                if (clients[i] == wsi) {
                    memmove(&clients[i], &clients[i + 1], 
                           (client_count - i - 1) * sizeof(struct lws *));
                    client_count--;
                    break;
                }
            }
            pthread_mutex_unlock(&sim_mutex);
            break;

        default:
            break;
    }
    return 0;
}

// 初始化同步问题
void init_problem(ProblemType type) {
    memset(&current_problem, 0, sizeof(SynchronizationProblem));
    current_problem.type = type;
    current_problem.speed = 1000; // 默认速度1秒
    current_problem.is_running = 0;
    
    switch (type) {
        case PROBLEM_PRODUCER_CONSUMER:
            setup_producer_consumer();
            break;
        case PROBLEM_READERS_WRITERS:
            setup_readers_writers();
            break;
        case PROBLEM_DINING_PHILOSOPHERS:
            setup_dining_philosophers();
            break;
        case PROBLEM_SLEEPING_BARBER:
            setup_sleeping_barber();
            break;
        case PROBLEM_SMOKERS:
            setup_smokers();
            break;
    }
}

// 生产者-消费者问题设置
void setup_producer_consumer() {
    strcpy(current_problem.name, "生产者-消费者问题");
    strcpy(current_problem.description, 
           "生产者向缓冲区生产数据，消费者从缓冲区消费数据");
    
    // 初始化缓冲区资源
    Resource buffer = {
        .id = 0,
        .type = RESOURCE_BUFFER,
        .capacity = 5,
        .used = 0,
        .is_mutex = 0
    };
    strcpy(buffer.name, "缓冲区");
    current_problem.resources[0] = buffer;
    current_problem.resource_count = 1;
    
    // 初始化生产者进程
    Process producer = {
        .id = 0,
        .role = ROLE_PRODUCER,
        .state = PROCESS_RUNNING
    };
    strcpy(producer.name, "生产者");
    current_problem.processes[0] = producer;
    
    // 初始化消费者进程
    Process consumer = {
        .id = 1,
        .role = ROLE_CONSUMER,
        .state = PROCESS_RUNNING
    };
    strcpy(consumer.name, "消费者");
    current_problem.processes[1] = consumer;
    current_problem.process_count = 2;
}

// 读者-写者问题设置
void setup_readers_writers() {
    strcpy(current_problem.name, "读者-写者问题");
    strcpy(current_problem.description, 
           "多个读者可以同时读取数据，但写者必须独占访问");
    
    // 初始化数据库资源
    Resource database = {
        .id = 0,
        .type = RESOURCE_DATABASE,
        .capacity = 1,
        .used = 0,
        .is_mutex = 1
    };
    strcpy(database.name, "数据库");
    current_problem.resources[0] = database;
    current_problem.resource_count = 1;
    
    // 初始化3个读者和2个写者
    for (int i = 0; i < 3; i++) {
        Process reader = {
            .id = i,
            .role = ROLE_READER,
            .state = PROCESS_RUNNING
        };
        sprintf(reader.name, "读者%d", i + 1);
        current_problem.processes[i] = reader;
    }
    
    for (int i = 0; i < 2; i++) {
        Process writer = {
            .id = i + 3,
            .role = ROLE_WRITER,
            .state = PROCESS_RUNNING
        };
        sprintf(writer.name, "写者%d", i + 1);
        current_problem.processes[i + 3] = writer;
    }
    current_problem.process_count = 5;
}

// 主函数
int main() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = WS_PORT;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    
    ws_context = lws_create_context(&info);
    if (!ws_context) {
        printf("WebSocket服务器创建失败\n");
        return -1;
    }
    
    printf("WebSocket服务器已启动，监听端口%d\n", WS_PORT);
    
    // 初始化默认问题
    init_problem(PROBLEM_PRODUCER_CONSUMER);
    
    // 创建模拟线程
    pthread_t sim_thread;
    pthread_create(&sim_thread, NULL, simulation_thread, NULL);
    
    // 主循环
    while (!should_exit) {
        lws_service(ws_context, 50);
    }
    
    // 清理资源
    if (ws_context) lws_context_destroy(ws_context);
    if (clients) free(clients);
    
    return 0;
}
