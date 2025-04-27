#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <jansson.h>

#define MAX_PROCESSES 100
#define PROCESS_NAME_LEN 50
#define WS_PORT 8888
#define BUFFER_SIZE 4096

// 进程状态定义
typedef enum {
    READY,
    RUNNING,
    WAITING,
    TERMINATED
} ProcessState;

// 进程控制块(PCB)结构
typedef struct {
    pid_t pid;                   // 进程ID
    char name[PROCESS_NAME_LEN]; // 进程名称
    ProcessState state;          // 进程状态
    int priority;                // 优先级（1-10，10为最高）
    pid_t ppid;                  // 父进程ID
    time_t creation_time;        // 创建时间
    int return_value;            // 返回值
} PCB;

// 全局变量
PCB process_table[MAX_PROCESSES];
int process_count = 0;
pthread_mutex_t process_table_mutex = PTHREAD_MUTEX_INITIALIZER;
struct lws_context *ws_context = NULL;
int should_exit = 0;

// 函数声明
void init_process_table();
int add_process(pid_t pid, const char* name, ProcessState state, int priority, pid_t ppid);
int find_process_by_pid(pid_t pid);
void update_process_state(pid_t pid, ProcessState state);
void update_process_return_value(pid_t pid, int return_value);
void remove_process(pid_t pid);
void display_process_table();
void execute_command(char* command, char* args[]);
void handle_client_message(const char* message);
void create_new_process(const char* name, int priority, int return_value);
void execute_client_command(const char* command, const char* name, int priority);
void terminate_client_process(pid_t pid);

// WebSocket服务器函数
void start_websocket_server();
void* websocket_thread(void* arg);
int callback_process_manager(struct lws *wsi, enum lws_callback_reasons reason, 
                             void *user, void *in, size_t len);
char* create_process_json();
void signal_handler(int sig);
void broadcast_process_table();

// 添加线程函数声明
void* wait_for_process(void* arg);
void* update_process_state_thread(void* arg);

// 定义WebSocket协议
static struct lws_protocols protocols[] = {
    {
        "process-manager-protocol",
        callback_process_manager,
        0,
        BUFFER_SIZE,
    },
    { NULL, NULL, 0, 0 } /* 终止项 */
};

// 所有连接的WebSocket客户端列表
struct lws **clients = NULL;
int client_count = 0;

int main() {
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    init_process_table();
    
    printf("进程管理系统已启动\n");
    printf("父进程PID: %d\n", getpid());
    
    // 添加主进程到进程表
    add_process(getpid(), "主进程", RUNNING, 10, getppid());
    
    // 启动WebSocket服务器线程
    pthread_t ws_thread;
    if (pthread_create(&ws_thread, NULL, websocket_thread, NULL) != 0) {
        perror("无法创建WebSocket服务器线程");
        return 1;
    }
    
    int choice;
    while (!should_exit) {
        printf("\n=== 进程管理系统 ===\n");
        printf("1. 创建新进程\n");
        printf("2. 创建并执行新命令\n");
        printf("3. 查看进程表\n");
        printf("4. 退出\n");
        printf("请选择操作 (1-4): ");
        
        scanf("%d", &choice);
        getchar(); // 消耗换行符
        
        switch (choice) {
            case 1: {
                char process_name[PROCESS_NAME_LEN];
                int priority, return_value;
                
                printf("输入进程名称: ");
                fgets(process_name, PROCESS_NAME_LEN, stdin);
                process_name[strcspn(process_name, "\n")] = 0; // 移除换行符
                
                printf("输入优先级 (1-10): ");
                scanf("%d", &priority);
                getchar(); // 消耗换行符
                
                printf("为子进程设置返回值: ");
                scanf("%d", &return_value);
                getchar(); // 消耗换行符
                
                pid_t child_pid = fork();
                
                if (child_pid < 0) {
                    perror("fork 失败");
                } else if (child_pid == 0) {
                    // 子进程代码
                    printf("\n[子进程] 已创建\n");
                    printf("[子进程] PID: %d\n", getpid());
                    printf("[子进程] 父进程PID: %d\n", getppid());
                    printf("[子进程] 名称: %s\n", process_name);
                    printf("[子进程] 优先级: %d\n", priority);
                    printf("[子进程] 将返回值设置为: %d\n", return_value);
                    printf("[子进程] 退出中...\n");
                    exit(return_value);
                } else {
                    // 父进程代码
                    printf("\n[父进程] 已创建子进程\n");
                    printf("[父进程] 子进程PID: %d\n", child_pid);
                    
                    // 添加子进程到进程表
                    pthread_mutex_lock(&process_table_mutex);
                    add_process(child_pid, process_name, READY, priority, getpid());
                    pthread_mutex_unlock(&process_table_mutex);
                    
                    // 广播进程表更新
                    broadcast_process_table();
                    
                    int status;
                    pid_t terminated_pid = waitpid(child_pid, &status, 0);
                    
                    if (terminated_pid == child_pid) {
                        int exit_status = WEXITSTATUS(status);
                        printf("[父进程] 子进程 %d 已退出，返回值: %d\n", child_pid, exit_status);
                        
                        pthread_mutex_lock(&process_table_mutex);
                        update_process_state(child_pid, TERMINATED);
                        update_process_return_value(child_pid, exit_status);
                        pthread_mutex_unlock(&process_table_mutex);
                        
                        // 广播进程表更新
                        broadcast_process_table();
                    }
                }
                break;
            }
            case 2: {
                char command[256];
                char process_name[PROCESS_NAME_LEN];
                int priority;
                
                printf("输入要执行的命令: ");
                fgets(command, 256, stdin);
                command[strcspn(command, "\n")] = 0; // 移除换行符
                
                printf("输入进程名称: ");
                fgets(process_name, PROCESS_NAME_LEN, stdin);
                process_name[strcspn(process_name, "\n")] = 0; // 移除换行符
                
                printf("输入优先级 (1-10): ");
                scanf("%d", &priority);
                getchar(); // 消耗换行符
                
                // 解析命令和参数
                char *args[64] = {NULL};
                char *token = strtok(command, " ");
                int i = 0;
                
                while (token != NULL && i < 63) {
                    args[i++] = token;
                    token = strtok(NULL, " ");
                }
                args[i] = NULL;
                
                pid_t child_pid = fork();
                
                if (child_pid < 0) {
                    perror("fork 失败");
                } else if (child_pid == 0) {
                    // 子进程代码
                    printf("\n[子进程] 已创建\n");
                    printf("[子进程] PID: %d\n", getpid());
                    printf("[子进程] 父进程PID: %d\n", getppid());
                    printf("[子进程] 名称: %s\n", process_name);
                    printf("[子进程] 优先级: %d\n", priority);
                    printf("[子进程] 执行命令: %s\n", command);
                    
                    execvp(args[0], args);
                    // 如果execvp返回，说明出错了
                    perror("execvp 失败");
                    exit(EXIT_FAILURE);
                } else {
                    // 父进程代码
                    printf("\n[父进程] 已创建子进程\n");
                    printf("[父进程] 子进程PID: %d\n", child_pid);
                    
                    // 添加子进程到进程表
                    pthread_mutex_lock(&process_table_mutex);
                    add_process(child_pid, process_name, RUNNING, priority, getpid());
                    pthread_mutex_unlock(&process_table_mutex);
                    
                    // 广播进程表更新
                    broadcast_process_table();
                    
                    int status;
                    pid_t terminated_pid = waitpid(child_pid, &status, 0);
                    
                    if (terminated_pid == child_pid) {
                        int exit_status = WEXITSTATUS(status);
                        printf("[父进程] 子进程 %d 已退出，返回值: %d\n", child_pid, exit_status);
                        
                        pthread_mutex_lock(&process_table_mutex);
                        update_process_state(child_pid, TERMINATED);
                        update_process_return_value(child_pid, exit_status);
                        pthread_mutex_unlock(&process_table_mutex);
                        
                        // 广播进程表更新
                        broadcast_process_table();
                    }
                }
                break;
            }
            case 3:
                pthread_mutex_lock(&process_table_mutex);
                display_process_table();
                pthread_mutex_unlock(&process_table_mutex);
                break;
            case 4:
                printf("正在退出进程管理系统...\n");
                should_exit = 1;
                
                // 等待WebSocket服务器线程退出
                pthread_join(ws_thread, NULL);
                return 0;
            default:
                printf("无效选择，请重试\n");
        }
    }
    
    return 0;
}

// 初始化进程表
void init_process_table() {
    process_count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].pid = -1;
    }
}

// 添加进程到进程表
int add_process(pid_t pid, const char* name, ProcessState state, int priority, pid_t ppid) {
    if (process_count >= MAX_PROCESSES) {
        return -1; // 进程表已满
    }
    
    int idx = process_count++;
    
    process_table[idx].pid = pid;
    strncpy(process_table[idx].name, name, PROCESS_NAME_LEN - 1);
    process_table[idx].name[PROCESS_NAME_LEN - 1] = '\0';
    process_table[idx].state = state;
    process_table[idx].priority = priority;
    process_table[idx].ppid = ppid;
    process_table[idx].creation_time = time(NULL);
    process_table[idx].return_value = -1; // 初始返回值
    
    return idx;
}

// 根据PID查找进程在进程表中的索引
int find_process_by_pid(pid_t pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid) {
            return i;
        }
    }
    return -1; // 未找到
}

// 更新进程状态
void update_process_state(pid_t pid, ProcessState state) {
    int idx = find_process_by_pid(pid);
    if (idx != -1) {
        process_table[idx].state = state;
    }
}

// 更新进程返回值
void update_process_return_value(pid_t pid, int return_value) {
    int idx = find_process_by_pid(pid);
    if (idx != -1) {
        process_table[idx].return_value = return_value;
    }
}

// 从进程表中移除进程
void remove_process(pid_t pid) {
    int idx = find_process_by_pid(pid);
    if (idx == -1) {
        return; // 进程不存在
    }
    
    // 移动后面的进程向前填补空缺
    for (int i = idx; i < process_count - 1; i++) {
        process_table[i] = process_table[i + 1];
    }
    
    process_count--;
}

// 显示进程状态的辅助函数
const char* get_state_name(ProcessState state) {
    switch (state) {
        case READY: return "就绪";
        case RUNNING: return "运行";
        case WAITING: return "等待";
        case TERMINATED: return "终止";
        default: return "未知";
    }
}

// 显示进程表
void display_process_table() {
    printf("\n======================= 进程表 =======================\n");
    printf("%-6s %-15s %-10s %-6s %-10s %-10s %-20s\n", 
           "PID", "名称", "状态", "优先级", "父PID", "返回值", "创建时间");
    printf("----------------------------------------------------------\n");
    
    char time_str[20];
    struct tm* time_info;
    
    for (int i = 0; i < process_count; i++) {
        PCB* p = &process_table[i];
        
        // 转换创建时间为可读格式
        time_info = localtime(&p->creation_time);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", time_info);
        
        printf("%-6d %-15s %-10s %-6d %-10d %-10d %-20s\n", 
               p->pid, p->name, get_state_name(p->state), p->priority, 
               p->ppid, p->return_value, time_str);
    }
    
    printf("进程总数: %d\n", process_count);
    printf("========================================================\n");
}

// 信号处理函数
void signal_handler(int sig) {
    printf("\n捕获信号 %d，正在退出...\n", sig);
    should_exit = 1;
    
    // 如果WebSocket上下文存在，触发中断以终止服务
    if (ws_context) {
        lws_cancel_service(ws_context);
    }
}

// 创建进程表的JSON表示
char* create_process_json() {
    json_t *root = json_object();
    json_t *processes_array = json_array();
    
    for (int i = 0; i < process_count; i++) {
        PCB* p = &process_table[i];
        
        json_t *process = json_object();
        json_object_set_new(process, "pid", json_integer(p->pid));
        json_object_set_new(process, "name", json_string(p->name));
        
        const char* state_str;
        switch (p->state) {
            case READY: state_str = "READY"; break;
            case RUNNING: state_str = "RUNNING"; break;
            case WAITING: state_str = "WAITING"; break;
            case TERMINATED: state_str = "TERMINATED"; break;
            default: state_str = "UNKNOWN"; break;
        }
        json_object_set_new(process, "state", json_string(state_str));
        
        json_object_set_new(process, "priority", json_integer(p->priority));
        json_object_set_new(process, "ppid", json_integer(p->ppid));
        json_object_set_new(process, "returnValue", json_integer(p->return_value));
        json_object_set_new(process, "creationTime", json_integer(p->creation_time));
        
        json_array_append_new(processes_array, process);
    }
    
    json_object_set_new(root, "processes", processes_array);
    
    char* json_str = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    
    return json_str;
}

// 处理客户端消息
void handle_client_message(const char* message) {
    // 查找JSON开始的地方
    const char *json_start = strstr(message, "{\"type\"");
    if (!json_start) {
        printf("无法找到有效的JSON开始: %s\n", message);
        return;
    }
    
    // 查找JSON结束的地方
    const char *json_end = strstr(json_start, "}}");
    if (!json_end) {
        printf("无法找到有效的JSON结束: %s\n", message);
        return;
    }
    json_end += 2; // 包含结束的 }}
    
    // 提取JSON部分
    size_t json_len = json_end - json_start;
    char *json_str = malloc(json_len + 1);
    memcpy(json_str, json_start, json_len);
    json_str[json_len] = '\0';
    
    printf("提取的JSON: %s\n", json_str);
    
    json_t *root;
    json_error_t error;
    
    root = json_loads(json_str, 0, &error);
    free(json_str);
    
    if (!root) {
        printf("JSON解析错误: %s\n", error.text);
        return;
    }
    
    // 获取消息类型和数据
    json_t *type_json = json_object_get(root, "type");
    json_t *data_json = json_object_get(root, "data");
    
    if (!json_is_string(type_json) || !json_is_object(data_json)) {
        printf("无效的消息格式: 缺少type或data字段\n");
        json_decref(root);
        return;
    }
    
    const char *type = json_string_value(type_json);
    
    if (strcmp(type, "createProcess") == 0) {
        // 处理创建进程的请求
        json_t *name_json = json_object_get(data_json, "name");
        json_t *priority_json = json_object_get(data_json, "priority");
        json_t *return_value_json = json_object_get(data_json, "returnValue");
        
        if (json_is_string(name_json) && json_is_integer(priority_json) && json_is_integer(return_value_json)) {
            const char *name = json_string_value(name_json);
            int priority = json_integer_value(priority_json);
            int return_value = json_integer_value(return_value_json);
            
            create_new_process(name, priority, return_value);
        }
    } else if (strcmp(type, "executeCommand") == 0) {
        // 处理执行命令的请求
        json_t *command_json = json_object_get(data_json, "command");
        json_t *name_json = json_object_get(data_json, "name");
        json_t *priority_json = json_object_get(data_json, "priority");
        
        if (json_is_string(command_json) && json_is_string(name_json) && json_is_integer(priority_json)) {
            const char *command = json_string_value(command_json);
            const char *name = json_string_value(name_json);
            int priority = json_integer_value(priority_json);
            
            execute_client_command(command, name, priority);
        }
    } else if (strcmp(type, "terminateProcess") == 0) {
        // 处理终止进程的请求
        json_t *pid_json = json_object_get(data_json, "pid");
        
        if (json_is_integer(pid_json)) {
            pid_t pid = json_integer_value(pid_json);
            terminate_client_process(pid);
        }
    } else {
        printf("未知的消息类型: %s\n", type);
    }
    
    json_decref(root);
}

// 创建新进程（从WebSocket客户端请求）
void create_new_process(const char* name, int priority, int return_value) {
    printf("从WebSocket创建新进程: %s, 优先级: %d, 返回值: %d\n", name, priority, return_value);
    
    pid_t child_pid = fork();
    
    if (child_pid < 0) {
        perror("fork 失败");
        return;
    } else if (child_pid == 0) {
        // 子进程代码
        printf("\n[子进程] 已创建 (从WebSocket)\n");
        printf("[子进程] PID: %d\n", getpid());
        printf("[子进程] 父进程PID: %d\n", getppid());
        printf("[子进程] 名称: %s\n", name);
        printf("[子进程] 优先级: %d\n", priority);
        printf("[子进程] 将返回值设置为: %d\n", return_value);
        printf("[子进程] 退出中...\n");
        
        // 模拟进程执行
        sleep(2);
        
        exit(return_value);
    } else {
        // 父进程代码
        printf("\n[父进程] 已创建子进程 (从WebSocket)\n");
        printf("[父进程] 子进程PID: %d\n", child_pid);
        
        // 添加子进程到进程表
        pthread_mutex_lock(&process_table_mutex);
        add_process(child_pid, name, READY, priority, getpid());
        pthread_mutex_unlock(&process_table_mutex);
        
        // 广播进程表更新
        broadcast_process_table();
        
        // 启动一个非阻塞等待子进程的线程
        pthread_t wait_thread;
        pid_t *pid_ptr = malloc(sizeof(pid_t));
        *pid_ptr = child_pid;
        
        pthread_create(&wait_thread, NULL, wait_for_process, pid_ptr);
        pthread_detach(wait_thread);
        
        // 模拟进程状态变化
        pthread_t state_thread;
        pid_t *state_pid_ptr = malloc(sizeof(pid_t));
        *state_pid_ptr = child_pid;
        
        pthread_create(&state_thread, NULL, update_process_state_thread, state_pid_ptr);
        pthread_detach(state_thread);
    }
}

// 执行命令（从WebSocket客户端请求）
void execute_client_command(const char* command, const char* name, int priority) {
    printf("从WebSocket执行命令: %s, 进程名: %s, 优先级: %d\n", command, name, priority);
    
    // 解析命令和参数
    char cmd_copy[256];
    strncpy(cmd_copy, command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    
    char *args[64] = {NULL};
    char *token = strtok(cmd_copy, " ");
    int i = 0;
    
    while (token != NULL && i < 63) {
        args[i++] = token;
        token = strtok(NULL, " ");
    }
    args[i] = NULL;
    
    pid_t child_pid = fork();
    
    if (child_pid < 0) {
        perror("fork 失败");
        return;
    } else if (child_pid == 0) {
        // 子进程代码
        printf("\n[子进程] 已创建 (从WebSocket)\n");
        printf("[子进程] PID: %d\n", getpid());
        printf("[子进程] 父进程PID: %d\n", getppid());
        printf("[子进程] 名称: %s\n", name);
        printf("[子进程] 优先级: %d\n", priority);
        printf("[子进程] 执行命令: %s\n", command);
        
        execvp(args[0], args);
        // 如果execvp返回，说明出错了
        perror("execvp 失败");
        exit(EXIT_FAILURE);
    } else {
        // 父进程代码
        printf("\n[父进程] 已创建子进程 (从WebSocket)\n");
        printf("[父进程] 子进程PID: %d\n", child_pid);
        
        // 添加子进程到进程表
        pthread_mutex_lock(&process_table_mutex);
        add_process(child_pid, name, RUNNING, priority, getpid());
        pthread_mutex_unlock(&process_table_mutex);
        
        // 广播进程表更新
        broadcast_process_table();
        
        // 启动一个非阻塞等待子进程的线程
        pthread_t wait_thread;
        pid_t *pid_ptr = malloc(sizeof(pid_t));
        *pid_ptr = child_pid;
        
        pthread_create(&wait_thread, NULL, wait_for_process, pid_ptr);
        pthread_detach(wait_thread);
    }
}

// 线程函数：等待进程终止
void* wait_for_process(void* arg) {
    pid_t pid = *((pid_t*)arg);
    free(arg);
    
    int status;
    pid_t terminated_pid = waitpid(pid, &status, 0);
    
    if (terminated_pid == pid) {
        int exit_status = WEXITSTATUS(status);
        printf("[父进程] 子进程 %d 已退出，返回值: %d\n", pid, exit_status);
        
        pthread_mutex_lock(&process_table_mutex);
        update_process_state(pid, TERMINATED);
        update_process_return_value(pid, exit_status);
        pthread_mutex_unlock(&process_table_mutex);
        
        // 广播进程表更新
        broadcast_process_table();
    }
    
    return NULL;
}

// 线程函数：更新进程状态
void* update_process_state_thread(void* arg) {
    pid_t pid = *((pid_t*)arg);
    free(arg);
    
    // 将进程状态更改为运行中
    sleep(1);
    pthread_mutex_lock(&process_table_mutex);
    update_process_state(pid, RUNNING);
    pthread_mutex_unlock(&process_table_mutex);
    broadcast_process_table();
    
    return NULL;
}

// 终止进程（从WebSocket客户端请求）
void terminate_client_process(pid_t pid) {
    printf("从WebSocket终止进程 PID: %d\n", pid);
    
    int idx = find_process_by_pid(pid);
    if (idx == -1) {
        printf("找不到PID为 %d 的进程\n", pid);
        return;
    }
    
    PCB* p = &process_table[idx];
    if (p->state == TERMINATED) {
        printf("进程 %d 已经终止\n", pid);
        return;
    }
    
    // 发送信号终止进程
    if (kill(pid, SIGTERM) == 0) {
        printf("成功发送SIGTERM信号到进程 %d\n", pid);
        
        // 等待进程终止
        int status;
        if (waitpid(pid, &status, WNOHANG) == 0) {
            // 如果进程没有立即终止，等待一段时间后强制终止
            sleep(1);
            if (kill(pid, SIGKILL) == 0) {
                printf("发送SIGKILL信号到进程 %d\n", pid);
                waitpid(pid, &status, 0);
            }
        }
        
        // 更新进程状态
        pthread_mutex_lock(&process_table_mutex);
        update_process_state(pid, TERMINATED);
        update_process_return_value(pid, WEXITSTATUS(status));
        pthread_mutex_unlock(&process_table_mutex);
        
        // 广播进程表更新
        broadcast_process_table();
    } else {
        perror("终止进程失败");
    }
}

// WebSocket回调函数
int callback_process_manager(struct lws *wsi, enum lws_callback_reasons reason, 
                           void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            printf("WebSocket连接已建立\n");
            
            // 添加到客户端列表
            clients = realloc(clients, (client_count + 1) * sizeof(struct lws *));
            clients[client_count++] = wsi;
            
            // 发送初始进程表
            pthread_mutex_lock(&process_table_mutex);
            char* json = create_process_json();
            pthread_mutex_unlock(&process_table_mutex);
            
            printf("发送初始进程表: %s\n", json);
            
            // 准备发送的数据，+LWS_PRE是libwebsockets要求的前置空间
            int json_len = strlen(json);
            unsigned char *buf = malloc(LWS_PRE + json_len);
            memcpy(buf + LWS_PRE, json, json_len);
            
            lws_write(wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
            
            free(buf);
            free(json);
            break;
        }
        case LWS_CALLBACK_RECEIVE: {
            // 处理从客户端接收的数据
            const char *received = (const char *)in;
            printf("收到原始消息 (长度 %zu): %s\n", len, received);
            
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
                printf("无法提取有效的JSON: %s\n", received);
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

// 广播进程表到所有连接的客户端
void broadcast_process_table() {
    if (client_count == 0) return;
    
    pthread_mutex_lock(&process_table_mutex);
    char* json = create_process_json();
    pthread_mutex_unlock(&process_table_mutex);
    
    printf("广播进程表更新: %s\n", json);
    
    int json_len = strlen(json);
    
    for (int i = 0; i < client_count; i++) {
        unsigned char *buf = malloc(LWS_PRE + json_len);
        memcpy(buf + LWS_PRE, json, json_len);
        
        lws_write(clients[i], buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
        
        free(buf);
    }
    
    free(json);
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
    info.vhost_name = "process-manager-server";
    info.ws_ping_pong_interval = 30;  // 设置心跳包间隔
    
    // 设置日志级别
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE;
    lws_set_log_level(logs, NULL);
    
    ws_context = lws_create_context(&info);
    if (!ws_context) {
        printf("创建WebSocket上下文失败\n");
        return NULL;
    }
    
    printf("WebSocket服务器已启动，监听端口 %d\n", WS_PORT);
    
    while (!should_exit) {
        lws_service(ws_context, 1000);
    }
    
    lws_context_destroy(ws_context);
    ws_context = NULL;
    
    if (clients) {
        free(clients);
        clients = NULL;
        client_count = 0;
    }
    
    printf("WebSocket服务器已关闭\n");
    return NULL;
}