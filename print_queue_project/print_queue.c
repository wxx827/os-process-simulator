#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/seq_file.h>

#define MAX_QUEUE_SIZE 10
#define NUM_PRODUCERS 3

typedef struct {
    int task_id;
} PrintTask;

PrintTask queue[MAX_QUEUE_SIZE];
int front = 0, rear = 0;
spinlock_t queue_lock;
struct semaphore sem_empty;
struct semaphore sem_full;

static int producer_thread(void *data);
static int consumer_thread(void *data);
static int print_queue_seq_show(struct seq_file *, void *);
static int print_queue_open(struct inode *, struct file *);

// 生产者线程（C90 声明规则）
static int producer_thread(void *data) {
    int id, task_id;  // 变量声明在函数开头
    id = *(int *)data;
    task_id = 0;
    while (!kthread_should_stop()) {
        down(&sem_empty);
        spin_lock(&queue_lock);
        queue[rear].task_id = task_id++;
        rear = (rear + 1) % MAX_QUEUE_SIZE;
        printk(KERN_INFO "Producer %d submitted task %d\n", id, queue[(rear - 1 + MAX_QUEUE_SIZE) % MAX_QUEUE_SIZE].task_id);
        spin_unlock(&queue_lock);
        up(&sem_full);
        msleep(1000);
    }
    return 0;
}

// 消费者线程（C90 声明规则）
static int consumer_thread(void *data) {
    int task_id;  // 变量声明在函数开头
    while (!kthread_should_stop()) {
        down(&sem_full);
        spin_lock(&queue_lock);
        task_id = queue[front].task_id;
        front = (front + 1) % MAX_QUEUE_SIZE;
        printk(KERN_INFO "Printer processing task %d\n", task_id);
        spin_unlock(&queue_lock);
        up(&sem_empty);
        msleep(2000);
    }
    return 0;
}

static struct task_struct *producer_tasks[NUM_PRODUCERS];
static struct task_struct *consumer_task;
static struct proc_dir_entry *print_queue_proc;

// 使用传统 file_operations（CentOS 7 内核要求）
static const struct file_operations print_queue_fops = {
    .open = print_queue_open,  // 打开文件时初始化 seq_file
    .read = seq_read,          // 读取文件内容
    .release = seq_release,    // 释放文件资源
};

// seq_file 初始化函数
static int print_queue_open(struct inode *inode, struct file *file) {
    return seq_open(file, &(struct seq_operations) {
        .show = print_queue_seq_show,  // 核心显示函数
    });
}

// 队列状态显示函数
static int print_queue_seq_show(struct seq_file *seq, void *v) {
    int i;  // 变量声明在函数开头
    spin_lock(&queue_lock);
    seq_printf(seq, "Print Task Queue:\n");
    seq_printf(seq, "-----------------\n");
    seq_printf(seq, "Queue Size: %d/%d\n", (rear - front + MAX_QUEUE_SIZE) % MAX_QUEUE_SIZE, MAX_QUEUE_SIZE);
    seq_printf(seq, "Tasks:\n");
    i = front;
    while (i != rear) {
        seq_printf(seq, "ID: %d\n", queue[i].task_id);
        i = (i + 1) % MAX_QUEUE_SIZE;
    }
    spin_unlock(&queue_lock);
    return 0;
}

static int __init print_queue_init(void) {
    int i, producer_ids[NUM_PRODUCERS];
    
    // 初始化锁和信号量
    spin_lock_init(&queue_lock);
    sema_init(&sem_empty, MAX_QUEUE_SIZE);
    sema_init(&sem_full, 0);
    
    // 创建 proc 文件（注意第四个参数为 file_operations 指针）
    print_queue_proc = proc_create("print_queue", 0444, NULL, &print_queue_fops);
    if (!print_queue_proc) {
        printk(KERN_ERR "Failed to create proc file\n");
        return -ENOMEM;
    }
    
    // 创建生产者线程
    for (i = 0; i < NUM_PRODUCERS; i++) {
        producer_ids[i] = i;
        producer_tasks[i] = kthread_run(producer_thread, &producer_ids[i], "producer%d", i);
        if (IS_ERR(producer_tasks[i])) {
            proc_remove(print_queue_proc);
            return PTR_ERR(producer_tasks[i]);
        }
    }
    
    // 创建消费者线程
    consumer_task = kthread_run(consumer_thread, NULL, "consumer");
    if (IS_ERR(consumer_task)) {
        proc_remove(print_queue_proc);
        return PTR_ERR(consumer_task);
    }
    
    printk(KERN_INFO "Print queue module initialized\n");
    return 0;
}

static void __exit print_queue_exit(void) {
    int i;
    // 停止所有线程
    for (i = 0; i < NUM_PRODUCERS; i++) {
        kthread_stop(producer_tasks[i]);
    }
    kthread_stop(consumer_task);
    // 删除 proc 文件
    proc_remove(print_queue_proc);
    printk(KERN_INFO "Print queue module unloaded\n");
}

module_init(print_queue_init);
module_exit(print_queue_exit);
MODULE_LICENSE("GPL");
