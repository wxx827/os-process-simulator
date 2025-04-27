## 项目概述

本项目实现了基于 **C语言** 的一套完整的 Linux 进程管理与调度系统模拟，涵盖了进程的创建、执行、状态管理与调度算法模拟，支持 WebSocket 通信，可视化展示进程状态变化。
 前端使用 HTML+CSS+JavaScript 技术，结合 Chart.js、Canvas 等进行动态可视化，后端采用多线程、信号量、进程控制、JSON序列化等技术，搭建了真实的操作系统核心机制简化版。

## 项目结构

- `c.c`：Linux进程管理系统（支持进程创建、命令执行、进程表广播）
- `c2.c`：进程调度算法模拟器（FCFS, SJF, Priority, Round Robin）
- `c3.c`：经典同步问题模拟器（生产者消费者、哲学家就餐、读者写者、睡眠理发师等）
- `os_work_01_look.html`：进程管理系统可视化前端界面
- `os_work_02_look.html`：进程调度算法可视化界面
- print_queue_project ：打印系统实现

## 核心功能

- **进程管理**：支持动态创建进程、执行指定命令、查看进程状态表。
- **进程调度**：
  - FCFS（先来先服务）
  - SJF（短作业优先）
  - Priority（优先级调度）
  - RR（时间片轮转）
- **同步问题模拟**：生产者-消费者、哲学家就餐、睡眠理发师等经典问题仿真。
- **WebSocket交互**：实时向前端推送进程表与调度状态。
- **可视化展示**：通过进程表、状态统计图、甘特图等方式，动态展示系统变化。

## 环境要求

- GCC（支持 pthread）
- libwebsockets 开发库
- jansson JSON 库
- Linux 操作系统环境（Ubuntu推荐）

```bash
sudo apt-get install libwebsockets-dev libjansson-dev
```

## 编译与运行

### 编译

```bash
gcc c.c -o process_manager -lpthread -lwebsockets -ljansson
gcc c2.c -o scheduler_simulator -lpthread -lwebsockets -ljansson
gcc c3.c -o sync_problem_simulator -lpthread -lwebsockets -ljansson
```

### 启动后端程序

例如：

```bash
./process_manager
./scheduler_simulator
./sync_problem_simulator
```

默认 WebSocket 端口分别为 `8888`（进程管理）与 `8080`（调度模拟/同步模拟）。

### 打开前端页面

在浏览器中打开：

- `os_work_01_look.html`：查看 **进程管理系统** 可视化
- `os_work_02_look.html`：查看 **调度算法模拟器** 可视化

> ⚡ 注意：确保前端连接的 WebSocket 地址与服务器实际IP和端口一致。

## 项目亮点

- 支持多种经典调度算法仿真，动态进程管理，真实还原操作系统关键机制。
- 引入 **WebSocket实时通信**，保证前端秒级刷新。
- 采用模块化设计，方便后期扩展更多调度算法和同步问题模型。
- 精美的前端界面，兼具统计分析与交互操作。

## TODO

- 支持更多调度算法（如多级反馈队列、EDF调度）
- 增加进程树绘制与优化
- 引入日志系统记录调度历史
- 前后端分离，支持Docker快速部署

## 作者

- **主开发者**：[王哈哈]
- **联系方式**：[[15502673966@163.com](]

