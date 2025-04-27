#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <jansson.h>

#define MAX_PROCESSES 20
#define MAX_QUEUE_SIZE 50
#define BUFFER_SIZE 4096
#define WS_PORT 8080

// 进程状态枚举
typedef enum {
    PROCESS_NEW,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_WAITING,
    PROCESS_TERMINATED
} ProcessState;

// 进程结构体
typedef struct {
    int id;                  // 进程ID
    char name[50];           // 进程名称
    int priority;            // 优先级（优先级调度使用）
    int burst_time;          // 总执行时间
    int remaining_time;      // 剩余执行时间
    int arrival_time;        // 到达时间
    int waiting_time;        // 等待时间
    int turnaround_time;     // 周转时间
    int completion_time;     // 完成时间
    ProcessState state;      // 进程状态
    int time_slice;          // 时间片（轮转调度使用）
} Process;

// 就绪队列结构体
typedef struct {
    Process *processes[MAX_QUEUE_SIZE];
    int front;
    int rear;
    int size;
} ReadyQueue;

// 全局变量
Process process_table[MAX_PROCESSES];
int process_count = 0;
ReadyQueue ready_queue;
Process *current_process = NULL;
int system_time = 0;
int quantum = 2;             // 默认时间片大小
char current_algorithm[20] = "FCFS";  // 默认调度算法

// 互斥锁
pthread_mutex_t scheduler_mutex = PTHREAD_MUTEX_INITIALIZER;

// WebSocket变量
struct lws_context *ws_context = NULL;
struct lws **clients = NULL;
int client_count = 0;
int should_exit = 0;

// 函数声明
void init_ready_queue();
int is_ready_queue_empty();
int is_ready_queue_full();
void enqueue_process(Process *process);
Process* dequeue_process();
Process* peek_process();
void create_process(const char *name, int priority, int burst_time, int arrival_time);
void update_process_state(int process_id, ProcessState state);
void fcfs_schedule();
void sjf_schedule();
void priority_schedule();
void round_robin_schedule();
void run_scheduler();
char* create_processes_json();
char* create_scheduler_state_json();
void broadcast_state();
void handle_client_message(const char* message);
void* scheduler_thread(void* arg);
void* wait_for_client_process(void* arg);

// WebSocket回调函数
int callback_scheduler(struct lws *wsi, enum lws_callback_reasons reason, 
                      void *user, void *in, size_t len);

// WebSocket协议定义
static struct lws_protocols protocols[] = {
    {
        "scheduler-protocol",
        callback_scheduler,
        0,
        BUFFER_SIZE,
    },
    { NULL, NULL, 0, 0 }
};

// 初始化就绪队列
void init_ready_queue() {
    ready_queue.front = 0;
    ready_queue.rear = -1;
    ready_queue.size = 0;
}

// 检查就绪队列是否为空
int is_ready_queue_empty() {
    return ready_queue.size == 0;
}

// 检查就绪队列是否已满
int is_ready_queue_full() {
    return ready_queue.size == MAX_QUEUE_SIZE;
}

// 将进程加入就绪队列
void enqueue_process(Process *process) {
    if (is_ready_queue_full()) {
        printf("就绪队列已满！\n");
        return;
    }
    
    ready_queue.rear = (ready_queue.rear + 1) % MAX_QUEUE_SIZE;
    ready_queue.processes[ready_queue.rear] = process;
    ready_queue.size++;
    
    process->state = PROCESS_READY;
}

// 从就绪队列取出进程
Process* dequeue_process() {
    if (is_ready_queue_empty()) {
        return NULL;
    }
    
    Process *process = ready_queue.processes[ready_queue.front];
    ready_queue.front = (ready_queue.front + 1) % MAX_QUEUE_SIZE;
    ready_queue.size--;
    
    return process;
}

// 查看就绪队列首位进程但不取出
Process* peek_process() {
    if (is_ready_queue_empty()) {
        return NULL;
    }
    
    return ready_queue.processes[ready_queue.front];
}

// 创建新进程
void create_process(const char *name, int priority, int burst_time, int arrival_time) {
    if (process_count >= MAX_PROCESSES) {
        printf("进程表已满！\n");
        return;
    }
    
    Process *process = &process_table[process_count++];
    process->id = process_count;
    strncpy(process->name, name, 49);
    process->name[49] = '\0';
    process->priority = priority;
    process->burst_time = burst_time;
    process->remaining_time = burst_time;
    process->arrival_time = arrival_time;
    process->waiting_time = 0;
    process->turnaround_time = 0;
    process->completion_time = 0;
    process->state = PROCESS_NEW;
    process->time_slice = quantum;
    
    printf("创建进程 %s (ID:%d) - 优先级:%d, 执行时间:%d, 到达时间:%d\n",
           process->name, process->id, process->priority, process->burst_time, process->arrival_time);
}

// 更新进程状态
void update_process_state(int process_id, ProcessState state) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].id == process_id) {
            process_table[i].state = state;
            break;
        }
    }
}

// 先来先服务(FCFS)调度算法
void fcfs_schedule() {
    // 检查是否有正在运行的进程
    if (current_process != NULL) {
        // 进程执行一个时间单位
        current_process->remaining_time--;
        
        // 检查进程是否执行完毕
        if (current_process->remaining_time <= 0) {
            printf("进程 %s (ID:%d) 执行完毕\n", current_process->name, current_process->id);
            current_process->state = PROCESS_TERMINATED;
            current_process->completion_time = system_time;
            current_process->turnaround_time = current_process->completion_time - current_process->arrival_time;
            current_process->waiting_time = current_process->turnaround_time - current_process->burst_time;
            current_process = NULL;
        }
    }
    
    // 如果没有正在运行的进程，从就绪队列选择一个
    if (current_process == NULL && !is_ready_queue_empty()) {
        current_process = dequeue_process();
        current_process->state = PROCESS_RUNNING;
        printf("进程 %s (ID:%d) 开始运行\n", current_process->name, current_process->id);
    }
    
    // 检查是否有新到达的进程
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].arrival_time == system_time && process_table[i].state == PROCESS_NEW) {
            printf("进程 %s (ID:%d) 到达系统\n", process_table[i].name, process_table[i].id);
            enqueue_process(&process_table[i]);
        }
    }
}

// 短作业优先(SJF)调度算法
void sjf_schedule() {
    // 检查是否有正在运行的进程
    if (current_process != NULL) {
        // 进程执行一个时间单位
        current_process->remaining_time--;
        
        // 检查进程是否执行完毕
        if (current_process->remaining_time <= 0) {
            printf("进程 %s (ID:%d) 执行完毕\n", current_process->name, current_process->id);
            current_process->state = PROCESS_TERMINATED;
            current_process->completion_time = system_time;
            current_process->turnaround_time = current_process->completion_time - current_process->arrival_time;
            current_process->waiting_time = current_process->turnaround_time - current_process->burst_time;
            current_process = NULL;
        }
    }
    
    // 检查是否有新到达的进程
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].arrival_time == system_time && process_table[i].state == PROCESS_NEW) {
            printf("进程 %s (ID:%d) 到达系统\n", process_table[i].name, process_table[i].id);
            process_table[i].state = PROCESS_READY;
            
            // 将新进程直接加入就绪队列
            enqueue_process(&process_table[i]);
        }
    }
    
    // 如果没有正在运行的进程，从就绪队列选择剩余时间最短的进程
    if (current_process == NULL && !is_ready_queue_empty()) {
        // 找出剩余时间最短的进程
        int shortest_idx = ready_queue.front;
        int min_time = ready_queue.processes[shortest_idx]->remaining_time;
        
        for (int i = 0; i < ready_queue.size; i++) {
            int idx = (ready_queue.front + i) % MAX_QUEUE_SIZE;
            if (ready_queue.processes[idx]->remaining_time < min_time) {
                min_time = ready_queue.processes[idx]->remaining_time;
                shortest_idx = idx;
            }
        }
        
        // 将最短进程移到队首
        if (shortest_idx != ready_queue.front) {
            Process *temp = ready_queue.processes[shortest_idx];
            
            // 移动队列元素
            for (int i = shortest_idx; i != ready_queue.front; i = (i - 1 + MAX_QUEUE_SIZE) % MAX_QUEUE_SIZE) {
                ready_queue.processes[i] = ready_queue.processes[(i - 1 + MAX_QUEUE_SIZE) % MAX_QUEUE_SIZE];
            }
            
            ready_queue.processes[ready_queue.front] = temp;
        }
        
        current_process = dequeue_process();
        current_process->state = PROCESS_RUNNING;
        printf("进程 %s (ID:%d) 开始运行（剩余时间：%d）\n", 
               current_process->name, current_process->id, current_process->remaining_time);
    }
}

// 优先级调度算法
void priority_schedule() {
    // 检查是否有正在运行的进程
    if (current_process != NULL) {
        // 进程执行一个时间单位
        current_process->remaining_time--;
        
        // 检查进程是否执行完毕
        if (current_process->remaining_time <= 0) {
            printf("进程 %s (ID:%d) 执行完毕\n", current_process->name, current_process->id);
            current_process->state = PROCESS_TERMINATED;
            current_process->completion_time = system_time;
            current_process->turnaround_time = current_process->completion_time - current_process->arrival_time;
            current_process->waiting_time = current_process->turnaround_time - current_process->burst_time;
            current_process = NULL;
        }
    }
    
    // 检查是否有新到达的进程
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].arrival_time == system_time && process_table[i].state == PROCESS_NEW) {
            printf("进程 %s (ID:%d) 到达系统\n", process_table[i].name, process_table[i].id);
            process_table[i].state = PROCESS_READY;
            
            // 将新进程直接加入就绪队列
            enqueue_process(&process_table[i]);
        }
    }
    
    // 如果没有正在运行的进程，从就绪队列选择优先级最高的进程
    if (current_process == NULL && !is_ready_queue_empty()) {
        // 找出优先级最高的进程（优先级数字越大，优先级越高）
        int highest_idx = ready_queue.front;
        int highest_priority = ready_queue.processes[highest_idx]->priority;
        
        for (int i = 0; i < ready_queue.size; i++) {
            int idx = (ready_queue.front + i) % MAX_QUEUE_SIZE;
            if (ready_queue.processes[idx]->priority > highest_priority) {
                highest_priority = ready_queue.processes[idx]->priority;
                highest_idx = idx;
            }
        }
        
        // 将最高优先级进程移到队首
        if (highest_idx != ready_queue.front) {
            Process *temp = ready_queue.processes[highest_idx];
            
            // 移动队列元素
            for (int i = highest_idx; i != ready_queue.front; i = (i - 1 + MAX_QUEUE_SIZE) % MAX_QUEUE_SIZE) {
                ready_queue.processes[i] = ready_queue.processes[(i - 1 + MAX_QUEUE_SIZE) % MAX_QUEUE_SIZE];
            }
            
            ready_queue.processes[ready_queue.front] = temp;
        }
        
        current_process = dequeue_process();
        current_process->state = PROCESS_RUNNING;
        printf("进程 %s (ID:%d) 开始运行（优先级：%d）\n", 
               current_process->name, current_process->id, current_process->priority);
    }
}

// 时间片轮转(RR)调度算法
void round_robin_schedule() {
    // 检查是否有正在运行的进程
    if (current_process != NULL) {
        // 进程执行一个时间单位
        current_process->remaining_time--;
        current_process->time_slice--;
        
        // 检查进程是否执行完毕
        if (current_process->remaining_time <= 0) {
            printf("进程 %s (ID:%d) 执行完毕\n", current_process->name, current_process->id);
            current_process->state = PROCESS_TERMINATED;
            current_process->completion_time = system_time;
            current_process->turnaround_time = current_process->completion_time - current_process->arrival_time;
            current_process->waiting_time = current_process->turnaround_time - current_process->burst_time;
            current_process = NULL;
        }
        // 检查时间片是否用完
        else if (current_process->time_slice <= 0) {
            printf("进程 %s (ID:%d) 时间片用完，重新加入就绪队列\n", current_process->name, current_process->id);
            current_process->state = PROCESS_READY;
            current_process->time_slice = quantum; // 重置时间片
            enqueue_process(current_process);
            current_process = NULL;
        }
    }
    
    // 检查是否有新到达的进程
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].arrival_time == system_time && process_table[i].state == PROCESS_NEW) {
            printf("进程 %s (ID:%d) 到达系统\n", process_table[i].name, process_table[i].id);
            enqueue_process(&process_table[i]);
        }
    }
    
    // 如果没有正在运行的进程，从就绪队列选择下一个进程
    if (current_process == NULL && !is_ready_queue_empty()) {
        current_process = dequeue_process();
        current_process->state = PROCESS_RUNNING;
        printf("进程 %s (ID:%d) 开始运行（剩余时间：%d，时间片：%d）\n", 
               current_process->name, current_process->id, current_process->remaining_time, current_process->time_slice);
    }
}

// 运行调度器
void run_scheduler() {
    printf("系统时间: %d\n", system_time);
    
    if (strcmp(current_algorithm, "FCFS") == 0) {
        fcfs_schedule();
    } else if (strcmp(current_algorithm, "SJF") == 0) {
        sjf_schedule();
    } else if (strcmp(current_algorithm, "Priority") == 0) {
        priority_schedule();
    } else if (strcmp(current_algorithm, "RR") == 0) {
        round_robin_schedule();
    } else {
        printf("未知调度算法: %s\n", current_algorithm);
        return;
    }
    
    // 更新系统时间
    system_time++;
    
    // 广播当前状态到所有客户端
    broadcast_state();
}

// 创建进程JSON数据
char* create_processes_json() {
    json_t *root = json_object();
    json_t *processes_array = json_array();
    
    for (int i = 0; i < process_count; i++) {
        Process *p = &process_table[i];
        
        json_t *process = json_object();
        json_object_set_new(process, "id", json_integer(p->id));
        json_object_set_new(process, "name", json_string(p->name));
        json_object_set_new(process, "priority", json_integer(p->priority));
        json_object_set_new(process, "burstTime", json_integer(p->burst_time));
        json_object_set_new(process, "remainingTime", json_integer(p->remaining_time));
        json_object_set_new(process, "arrivalTime", json_integer(p->arrival_time));
        json_object_set_new(process, "waitingTime", json_integer(p->waiting_time));
        json_object_set_new(process, "turnaroundTime", json_integer(p->turnaround_time));
        json_object_set_new(process, "completionTime", json_integer(p->completion_time));
        
        const char* state_str;
        switch (p->state) {
            case PROCESS_NEW: state_str = "NEW"; break;
            case PROCESS_READY: state_str = "READY"; break;
            case PROCESS_RUNNING: state_str = "RUNNING"; break;
            case PROCESS_WAITING: state_str = "WAITING"; break;
            case PROCESS_TERMINATED: state_str = "TERMINATED"; break;
            default: state_str = "UNKNOWN"; break;
        }
        json_object_set_new(process, "state", json_string(state_str));
        json_object_set_new(process, "timeSlice", json_integer(p->time_slice));
        
        json_array_append_new(processes_array, process);
    }
    
    json_object_set_new(root, "processes", processes_array);
    
    char* json_str = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    
    return json_str;
}

// 创建调度器状态JSON数据
char* create_scheduler_state_json() {
    json_t *root = json_object();
    
    // 添加系统时间
    json_object_set_new(root, "systemTime", json_integer(system_time));
    
    // 添加当前调度算法
    json_object_set_new(root, "algorithm", json_string(current_algorithm));
    
    // 添加时间片大小
    json_object_set_new(root, "quantum", json_integer(quantum));
    
    // 添加当前运行进程
    if (current_process != NULL) {
        json_t *running = json_object();
        json_object_set_new(running, "id", json_integer(current_process->id));
        json_object_set_new(running, "name", json_string(current_process->name));
        json_object_set_new(running, "remainingTime", json_integer(current_process->remaining_time));
        json_object_set_new(running, "timeSlice", json_integer(current_process->time_slice));
        json_object_set_new(root, "runningProcess", running);
    } else {
        json_object_set_new(root, "runningProcess", json_null());
    }
    
    // 添加就绪队列
    json_t *ready_array = json_array();
    for (int i = 0; i < ready_queue.size; i++) {
        int idx = (ready_queue.front + i) % MAX_QUEUE_SIZE;
        Process *p = ready_queue.processes[idx];
        
        json_t *ready_process = json_object();
        json_object_set_new(ready_process, "id", json_integer(p->id));
        json_object_set_new(ready_process, "name", json_string(p->name));
        json_object_set_new(ready_process, "priority", json_integer(p->priority));
        json_object_set_new(ready_process, "remainingTime", json_integer(p->remaining_time));
        
        json_array_append_new(ready_array, ready_process);
    }
    json_object_set_new(root, "readyQueue", ready_array);
    
    // 添加进程统计
    int total = process_count;
    int new_count = 0, ready_count = ready_queue.size, running_count = (current_process != NULL) ? 1 : 0;
    int waiting_count = 0, terminated_count = 0;
    
    for (int i = 0; i < process_count; i++) {
        switch (process_table[i].state) {
            case PROCESS_NEW: new_count++; break;
            case PROCESS_WAITING: waiting_count++; break;
            case PROCESS_TERMINATED: terminated_count++; break;
            default: break;
        }
    }
    
    json_t *stats = json_object();
    json_object_set_new(stats, "total", json_integer(total));
    json_object_set_new(stats, "new", json_integer(new_count));
    json_object_set_new(stats, "ready", json_integer(ready_count));
    json_object_set_new(stats, "running", json_integer(running_count));
    json_object_set_new(stats, "waiting", json_integer(waiting_count));
    json_object_set_new(stats, "terminated", json_integer(terminated_count));
    
    json_object_set_new(root, "statistics", stats);
    
    // 将所有信息合并为单个JSON对象
    char* json_str = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    
    return json_str;
}

// 广播当前状态到所有客户端
void broadcast_state() {
    if (client_count == 0) return;
    
    char* processes_json = create_processes_json();
    char* scheduler_json = create_scheduler_state_json();
    
    json_t *root = json_object();
    json_object_set_new(root, "type", json_string("state_update"));
    
    json_error_t error;
    json_t *processes_obj = json_loads(processes_json, 0, &error);
    json_t *scheduler_obj = json_loads(scheduler_json, 0, &error);
    
    if (processes_obj && scheduler_obj) {
        json_t *data = json_object();
        json_object_update(data, processes_obj);
        json_object_update(data, scheduler_obj);
        json_object_set_new(root, "data", data);
        
        char* full_json = json_dumps(root, JSON_COMPACT);
        int json_len = strlen(full_json);
        
        printf("广播状态: %s\n", full_json);
        
        for (int i = 0; i < client_count; i++) {
            unsigned char *buf = malloc(LWS_PRE + json_len);
            memcpy(buf + LWS_PRE, full_json, json_len);
            
            lws_write(clients[i], buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
            
            free(buf);
        }
        
        free(full_json);
        json_decref(root);
    }
    
    free(processes_json);
    free(scheduler_json);
}

// 处理客户端消息
void handle_client_message(const char* message) {
    json_t *root;
    json_error_t error;
    
    printf("处理客户端消息: %s\n", message);
    
    root = json_loads(message, 0, &error);
    if (!root) {
        printf("JSON解析错误: %s\n", error.text);
        return;
    }
    
    json_t *type_json = json_object_get(root, "type");
    json_t *data_json = json_object_get(root, "data");
    
    if (!json_is_string(type_json)) {
        printf("无效的消息格式: 缺少type字段\n");
        json_decref(root);
        return;
    }
    
    const char *type = json_string_value(type_json);
    
    if (strcmp(type, "create_process") == 0) {
        if (!json_is_object(data_json)) {
            printf("创建进程消息无效: 缺少data字段\n");
            json_decref(root);
            return;
        }
        
        json_t *name_json = json_object_get(data_json, "name");
        json_t *priority_json = json_object_get(data_json, "priority");
        json_t *burst_time_json = json_object_get(data_json, "burstTime");
        json_t *arrival_time_json = json_object_get(data_json, "arrivalTime");
        
        if (json_is_string(name_json) && json_is_integer(priority_json) &&
            json_is_integer(burst_time_json) && json_is_integer(arrival_time_json)) {
            
            const char *name = json_string_value(name_json);
            int priority = json_integer_value(priority_json);
            int burst_time = json_integer_value(burst_time_json);
            int arrival_time = json_integer_value(arrival_time_json);
            
            pthread_mutex_lock(&scheduler_mutex);
            create_process(name, priority, burst_time, arrival_time);
            pthread_mutex_unlock(&scheduler_mutex);
            
            broadcast_state();
        }
    } else if (strcmp(type, "set_algorithm") == 0) {
        if (!json_is_object(data_json)) {
            printf("设置算法消息无效: 缺少data字段\n");
            json_decref(root);
            return;
        }
        
        json_t *algorithm_json = json_object_get(data_json, "algorithm");
        
        if (json_is_string(algorithm_json)) {
            const char *algorithm = json_string_value(algorithm_json);
            
            pthread_mutex_lock(&scheduler_mutex);
            strncpy(current_algorithm, algorithm, sizeof(current_algorithm) - 1);
            current_algorithm[sizeof(current_algorithm) - 1] = '\0';
            pthread_mutex_unlock(&scheduler_mutex);
            
            printf("调度算法已更改为: %s\n", current_algorithm);
            broadcast_state();
        }
    } else if (strcmp(type, "set_quantum") == 0) {
        if (!json_is_object(data_json)) {
            printf("设置时间片消息无效: 缺少data字段\n");
            json_decref(root);
            return;
        }
        
        json_t *quantum_json = json_object_get(data_json, "quantum");
        
        if (json_is_integer(quantum_json)) {
            int new_quantum = json_integer_value(quantum_json);
            
            if (new_quantum > 0) {
                pthread_mutex_lock(&scheduler_mutex);
                quantum = new_quantum;
                pthread_mutex_unlock(&scheduler_mutex);
                
                printf("时间片已更改为: %d\n", quantum);
                broadcast_state();
            }
        }
    } else if (strcmp(type, "reset_simulation") == 0) {
        pthread_mutex_lock(&scheduler_mutex);
        
        // 重置系统状态
        process_count = 0;
        init_ready_queue();
        current_process = NULL;
        system_time = 0;
        
        pthread_mutex_unlock(&scheduler_mutex);
        
        printf("模拟已重置\n");
        broadcast_state();
    } else if (strcmp(type, "start_simulation") == 0) {
        // 启动自动调度
        pthread_t sim_thread;
        int *speed_ptr = malloc(sizeof(int));
        
        // 获取速度参数
        json_t *speed_json = json_object_get(data_json, "speed");
        *speed_ptr = json_is_integer(speed_json) ? json_integer_value(speed_json) : 1000; // 默认1秒
        
        printf("开始模拟，速度: %d ms\n", *speed_ptr);
        
        pthread_create(&sim_thread, NULL, scheduler_thread, speed_ptr);
        pthread_detach(sim_thread);
    } else if (strcmp(type, "step_simulation") == 0) {
        // 单步执行调度
        pthread_mutex_lock(&scheduler_mutex);
        run_scheduler();
        pthread_mutex_unlock(&scheduler_mutex);
    } else {
        printf("未知消息类型: %s\n", type);
    }
    
    json_decref(root);
}

// 调度器线程函数
void* scheduler_thread(void* arg) {
    int speed = *(int*)arg;
    free(arg);
    
    int running = 1;
    int all_terminated = 0;
    
    while (running && !should_exit) {
        // 检查是否所有进程都已终止
        all_terminated = 1;
        for (int i = 0; i < process_count; i++) {
            if (process_table[i].state != PROCESS_TERMINATED) {
                all_terminated = 0;
                break;
            }
        }
        
        if (all_terminated && process_count > 0) {
            printf("所有进程已完成执行\n");
            break;
        }
        
        pthread_mutex_lock(&scheduler_mutex);
        run_scheduler();
        pthread_mutex_unlock(&scheduler_mutex);
        
        // 休眠指定时间
        usleep(speed * 1000); // 转换为微秒
    }
    
    return NULL;
}

// WebSocket回调函数
int callback_scheduler(struct lws *wsi, enum lws_callback_reasons reason, 
                      void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            printf("WebSocket连接已建立\n");
            
            // 添加到客户端列表
            clients = realloc(clients, (client_count + 1) * sizeof(struct lws *));
            clients[client_count++] = wsi;
            
            // 发送初始状态
            broadcast_state();
            break;
        }
        case LWS_CALLBACK_RECEIVE: {
            // 处理从客户端接收的数据
            const char *received = (const char *)in;
            
            // 查找JSON开始的地方
            const char *json_start = received;
            while (*json_start && *json_start != '{') {
                json_start++;
            }
            
            // 查找JSON结束的地方
            const char *json_end = received + len - 1;
            while (json_end > json_start && *json_end != '}') {
                json_end--;
            }
            
            // 如果找到了完整的JSON
            if (*json_start == '{' && *json_end == '}') {
                // 复制JSON部分
                size_t json_len = json_end - json_start + 1;
                char *json_str = malloc(json_len + 1);
                memcpy(json_str, json_start, json_len);
                json_str[json_len] = '\0';
                
                printf("提取的JSON: %s\n", json_str);
                
                // 处理客户端命令
                handle_client_message(json_str);
                
                free(json_str);
            } else {
                printf("无法提取有效的JSON: %.*s\n", (int)len, received);
            }
            
            break;
        }
        case LWS_CALLBACK_CLOSED: {
            printf("WebSocket连接已关闭\n");
            
            // 从客户端列表中移除
            for (int i = 0; i < client_count; i++) {
                if (clients[i] == wsi) {
                    for (int j = i; j < client_count - 1; j++) {
                        clients[j] = clients[j + 1];
                    }
                    client_count--;
                    clients = realloc(clients, client_count * sizeof(struct lws *));
                    break;
                }
            }
            break;
        }
        default:
            break;
    }
    
    return 0;
}

// WebSocket服务线程
void* websocket_thread(void* arg) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = WS_PORT;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.iface = "0.0.0.0";  // 绑定到所有网络接口，允许外部连接
    info.vhost_name = "scheduler-server";
    info.ws_ping_pong_interval = 30;  // 设置心跳包间隔
    
    // 设置日志级别
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE;
    lws_set_log_level(logs, NULL);
    
    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        printf("创建WebSocket上下文失败\n");
        return NULL;
    }
    
    ws_context = context;
    
    printf("WebSocket服务器已启动，监听端口 %d\n", WS_PORT);
    
    while (!should_exit) {
        lws_service(context, 1000);
    }
    
    lws_context_destroy(context);
    ws_context = NULL;
    
    if (clients) {
        free(clients);
        clients = NULL;
        client_count = 0;
    }
    
    printf("WebSocket服务器已关闭\n");
    return NULL;
}

// 处理信号
void signal_handler(int sig) {
    printf("\n捕获信号 %d，正在退出...\n", sig);
    should_exit = 1;
    
    // 如果WebSocket上下文存在，触发中断以终止服务
    if (ws_context) {
        lws_cancel_service(ws_context);
    }
}

int main() {
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 初始化进程队列
    init_ready_queue();
    
    printf("进程调度模拟器已启动\n");
    
    // 添加一些测试进程
    create_process("进程A", 5, 8, 0);
    create_process("进程B", 3, 4, 1);
    create_process("进程C", 7, 6, 2);
    create_process("进程D", 2, 2, 3);
    
    // 启动WebSocket服务器线程
    pthread_t ws_thread;
    if (pthread_create(&ws_thread, NULL, websocket_thread, NULL) != 0) {
        perror("无法创建WebSocket服务器线程");
        return 1;
    }
    
    // 主循环
    while (!should_exit) {
        printf("\n=== 进程调度模拟器 ===\n");
        printf("1. 创建新进程\n");
        printf("2. 显示进程表\n");
        printf("3. 设置调度算法\n");
        printf("4. 设置时间片大小\n");
        printf("5. 单步执行调度\n");
        printf("6. 自动执行调度\n");
        printf("7. 重置模拟器\n");
        printf("8. 退出\n");
        printf("请选择操作 (1-8): ");
        
        int choice;
        scanf("%d", &choice);
        getchar(); // 消耗换行符
        
        switch (choice) {
            case 1: {
                char name[50];
                int priority, burst_time, arrival_time;
                
                printf("输入进程名称: ");
                fgets(name, sizeof(name), stdin);
                name[strcspn(name, "\n")] = 0; // 移除换行符
                
                printf("输入优先级 (1-10): ");
                scanf("%d", &priority);
                getchar();
                
                printf("输入执行时间: ");
                scanf("%d", &burst_time);
                getchar();
                
                printf("输入到达时间: ");
                scanf("%d", &arrival_time);
                getchar();
                
                pthread_mutex_lock(&scheduler_mutex);
                create_process(name, priority, burst_time, arrival_time);
                pthread_mutex_unlock(&scheduler_mutex);
                
                broadcast_state();
                break;
            }
            case 2: {
                pthread_mutex_lock(&scheduler_mutex);
                
                printf("\n======================= 进程表 =======================\n");
                printf("%-4s %-15s %-8s %-6s %-12s %-12s %-8s %-8s %-12s %-8s\n", 
                       "ID", "名称", "状态", "优先级", "执行时间", "剩余时间", "到达时间",
                       "等待时间", "周转时间", "完成时间");
                printf("----------------------------------------------------------\n");
                
                for (int i = 0; i < process_count; i++) {
                    Process *p = &process_table[i];
                    
                    const char* state_str;
                    switch (p->state) {
                        case PROCESS_NEW: state_str = "新建"; break;
                        case PROCESS_READY: state_str = "就绪"; break;
                        case PROCESS_RUNNING: state_str = "运行"; break;
                        case PROCESS_WAITING: state_str = "等待"; break;
                        case PROCESS_TERMINATED: state_str = "终止"; break;
                        default: state_str = "未知"; break;
                    }
                    
                    printf("%-4d %-15s %-8s %-6d %-12d %-12d %-8d %-8d %-12d %-8d\n", 
                           p->id, p->name, state_str, p->priority, p->burst_time, p->remaining_time,
                           p->arrival_time, p->waiting_time, p->turnaround_time, p->completion_time);
                }
                
                printf("\n系统时间: %d\n", system_time);
                printf("当前算法: %s\n", current_algorithm);
                printf("时间片大小: %d\n", quantum);
                
                if (current_process != NULL) {
                    printf("当前运行进程: %s (ID:%d)\n", current_process->name, current_process->id);
                } else {
                    printf("当前运行进程: 无\n");
                }
                
                printf("就绪队列大小: %d\n", ready_queue.size);
                
                pthread_mutex_unlock(&scheduler_mutex);
                break;
            }
            case 3: {
                printf("选择调度算法:\n");
                printf("1. 先来先服务 (FCFS)\n");
                printf("2. 短作业优先 (SJF)\n");
                printf("3. 优先级调度 (Priority)\n");
                printf("4. 时间片轮转 (RR)\n");
                printf("请选择 (1-4): ");
                
                int alg_choice;
                scanf("%d", &alg_choice);
                getchar();
                
                pthread_mutex_lock(&scheduler_mutex);
                
                switch (alg_choice) {
                    case 1:
                        strcpy(current_algorithm, "FCFS");
                        break;
                    case 2:
                        strcpy(current_algorithm, "SJF");
                        break;
                    case 3:
                        strcpy(current_algorithm, "Priority");
                        break;
                    case 4:
                        strcpy(current_algorithm, "RR");
                        break;
                    default:
                        printf("无效选择，保持当前算法: %s\n", current_algorithm);
                }
                
                printf("调度算法已设置为: %s\n", current_algorithm);
                
                pthread_mutex_unlock(&scheduler_mutex);
                
                broadcast_state();
                break;
            }
            case 4: {
                printf("当前时间片大小: %d\n", quantum);
                printf("输入新的时间片大小: ");
                
                int new_quantum;
                scanf("%d", &new_quantum);
                getchar();
                
                if (new_quantum > 0) {
                    pthread_mutex_lock(&scheduler_mutex);
                    quantum = new_quantum;
                    pthread_mutex_unlock(&scheduler_mutex);
                    
                    printf("时间片大小已设置为: %d\n", quantum);
                    
                    broadcast_state();
                } else {
                    printf("无效的时间片大小，必须大于0\n");
                }
                break;
            }
            case 5: {
                pthread_mutex_lock(&scheduler_mutex);
                run_scheduler();
                pthread_mutex_unlock(&scheduler_mutex);
                break;
            }
            case 6: {
                printf("输入调度速度(毫秒): ");
                int speed;
                scanf("%d", &speed);
                getchar();
                
                if (speed <= 0) {
                    speed = 1000; // 默认1秒
                }
                
                pthread_t sim_thread;
                int *speed_ptr = malloc(sizeof(int));
                *speed_ptr = speed;
                
                pthread_create(&sim_thread, NULL, scheduler_thread, speed_ptr);
                pthread_detach(sim_thread);
                
                printf("自动调度已启动，速度: %d 毫秒\n", speed);
                break;
            }
            case 7: {
                pthread_mutex_lock(&scheduler_mutex);
                
                // 重置系统状态
                process_count = 0;
                init_ready_queue();
                current_process = NULL;
                system_time = 0;
                
                pthread_mutex_unlock(&scheduler_mutex);
                
                printf("模拟器已重置\n");
                broadcast_state();
                break;
            }
            case 8:
                printf("正在退出...\n");
                should_exit = 1;
                
                if (ws_context) {
                    lws_cancel_service(ws_context);
                }
                
                // 等待WebSocket服务器线程退出
                pthread_join(ws_thread, NULL);
                return 0;
            default:
                printf("无效选择，请重试\n");
        }
    }
    
    return 0;
}
