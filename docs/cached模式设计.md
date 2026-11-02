# C++ 线程池项目核心总结：`Cached` 动态扩缩容模式设计与源码剖析

在线程池的工业级实现中，线程数量的管理通常分为两种核心模式：**`MODE_FIXED`（固定大小模式）** 与 **`MODE_CACHED`（动态缓存模式）**。本文结合项目源码，从**设计动机、数据结构改造、动态扩容、超时回收、常见 Bug 排查**五个维度，完整拆解 `Cached` 模式的底层逻辑。

---

## 一、 核心设计思想：为什么需要 `Cached` 模式？

### 1. 两种模式的场景对比
| 特性维度 | `MODE_FIXED`（固定线程模式） | `MODE_CACHED`（动态扩缩容模式） |
| :--- | :--- | :--- |
| **线程数量** | 启动后恒定为 `initThreadSize_`，运行期不变 | 在 `[initThreadSize_, threadSizeThreshHold_]` 区间内动态伸缩 |
| **适用场景** | 耗时较长、并发量相对平稳的后台任务（防过多上下文切换） | **小而快的突发洪峰任务（Short-lived & Bursty Tasks）** |
| **核心痛点** | 突发大量任务时，队列积压严重，响应延迟高 | 扩容后若长期无任务，过多闲置线程会浪费系统内存与调度资源 |

### 2. `Cached` 模式的生命周期闭环
为了兼顾**“高并发突发响应”**与**“低负载资源节约”**，`Cached` 模式围绕以下两个核心规则构建闭环：
1. **按需动态扩容（生产者端 `submitTask` 触发）：**
   当用户提交任务时，若发现**当前队列中积压的未处理任务数 $>$ 当前空闲线程数**，且**当前总线程数 $<$ 线程上限阈值**，则当场创建并启动新线程以加速消费。
2. **空闲超时回收（消费者端 `threadFunc` 触发）：**
   当突发流量过去后，若某个工作线程**连续空闲时间达到阈值（如 $60\text{s}$，测试常设 $10\text{s}$）**，且**当前总线程数 $>$ 初始核心线程数（`initThreadSize_`）**，则该多余线程主动将自身从线程池中注销并安全退出，最终使线程数回落至 `initThreadSize_`。

---

## 二、 数据结构与成员变量改造（为什么要这样设计？）

在从 `Fixed` 模式演进到 `Cached` 模式时，头文件 `ThreadPool.h` 做了三处关键的数据结构设计：

### 1. 线程容器：为什么从 `std::vector` 改为 `std::unordered_map`？
```cpp
// Fixed 模式下只需顺序遍历启动，用 vector 即可：
// std::vector<std::unique_ptr<Thread>> threads_;

// Cached 模式下必须改为哈希表，建立 <自定义线程ID, 线程对象指针> 的映射：
std::unordered_map<int, std::unique_ptr<Thread>> threads_;
```
* **设计原因：**
  在 `Fixed` 模式中，所有线程同生共死，不需要单独删除某一个线程。而在 `Cached` 模式中，某个工作线程在 `threadFunc` 内部检测到自己空闲满 $60\text{s}$ 后，需要**在运行期精准地把自己从 `threads_` 容器中移除**。
  如果使用 `std::vector`，线程不仅难以定位自身对应的元素下标，而且删除中间元素的时间复杂度为 $O(N)$。改用 `std::unordered_map<int, std::unique_ptr<Thread>>` 后，工作线程只需知道自己的 `threadId`，即可通过 `threads_.erase(threadId)` 以 **$O(1)$ 时间复杂度**完成定点移除与 `Thread` 包装对象的析构。

### 2. 线程 ID 传递机制：`Thread` 类的改造
为了让工作线程在执行 `ThreadPool::threadFunc` 时知道自己在 `threads_` 哈希表中的 Key，不能直接使用操作系统的 `std::this_thread::get_id()`（类型复杂且不直观），因此项目在 `Thread` 包装类中引入了自增 ID 并通过 `std::bind` 占位符透传：

```cpp
class Thread
{
public:
    // 1. 线程函数签名增加 int 参数，用于接收自定义 threadId_
    using ThreadFunc = std::function<void(int)>;

    Thread(ThreadFunc func)
        : func_(func)
        , threadId_(generateId_++) // 2. 每次构造新线程，静态变量自增分配唯一 ID
    { }

    void start() {
        // 3. 启动底层线程时，将自身的 threadId_ 作为实参传入 func_（即 threadFunc）
        std::thread t(func_, threadId_);
        t.detach(); // 设置为分离线程，内核自动回收底层线程资源
    }

    int getId() const { return threadId_; }

private:
    ThreadFunc func_;
    static int generateId_; // 静态全局计数器
    int threadId_;          // 当前线程对象的唯一编号
};
```

### 3. 核心状态计数变量（原子类型保障并发安全）
```cpp
int initThreadSize_;             // 初始核心线程数量（缩容底线）
int threadSizeThreshHold_;       // Cached 模式下线程数量上限阈值（扩容天花板）
std::atomic_int curThreadSize_;  // 当前线程池中的总线程数量
std::atomic_int idleThreadSize_; // 当前处于空闲等待状态的线程数量
std::atomic_int taskSize_;       // 当前任务队列中排队未取走的任务数量
```
* **为什么 `curThreadSize_`、`idleThreadSize_`、`taskSize_` 要用 `std::atomic_int`？**
  因为主线程（调用 `submitTask`）与多个工作子线程（执行 `threadFunc`）会跨线程并发读写这些计数（例如任务执行完毕后在锁作用域外部执行 `idleThreadSize_++`），使用原子类型可以保证自增/自减操作的原子性与多核 CPU 缓存可见性。

---

## 三、 阶段一：任务提交时的动态扩容机制（`submitTask` 源码剖析）

当用户调用 `submitTask` 提交任务时，除了将任务入队并通知工作线程外，还承担了 `Cached` 模式的**动态扩容**职责。

### 1. 源码实现与逐行注释
```cpp
Result ThreadPool::submitTask(std::shared_ptr<Task> sp) {
    // 1. 获取互斥锁，保护任务队列与线程容器的并发访问
    std::unique_lock<std::mutex> unique_lock(taskQueMtx_);

    // 2. 等待队列有空余位置（最多阻塞 1 秒，超时则判定提交失败）
    bool hasSpace = notFull.wait_for(unique_lock, std::chrono::seconds(1), [this]() {
        return taskSize_ < taskQueMaxThreshHold_;
    });
    if (!hasSpace) {
        std::cerr << "task queue is Full, submit task failed." << std::endl;
        return Result(sp, false);
    }

    // 3. 任务入队，未处理任务计数 +1
    taskQue_.push(sp);
    taskSize_++;

    // 4. 唤醒一个/所有正在等待任务的消费线程
    notEmpty.notify_all();

    // 5. 【Cached 模式核心扩容逻辑】
    // 条件①：当前为 MODE_CACHED 模式
    // 条件②：未消费的任务数 > 当前空闲线程数（说明现有空闲线程不够分了）
    // 条件③：当前总线程数 < 线程池允许的最大线程阈值（防止无限创建耗尽系统资源）
    if (poolMode_ == PoolMode::MODE_CACHED
        && taskSize_ > idleThreadSize_
        && curThreadSize_ < threadSizeThreshHold_) 
    {
        std::cout << ">>> create new thread " << std::this_thread::get_id() << std::endl;

        // 5.1 创建新 Thread 对象，使用 placeholders::_1 预留 threadId 参数位置
        auto ptr = std::make_unique<Thread>(
            std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        
        // 5.2 先提取 threadId（必须在 std::move(ptr) 之前提取！）
        int threadId = ptr->getId();
        
        // 5.3 将独占指针转移进哈希表管理
        threads_.emplace(threadId, std::move(ptr));
        
        // 5.4 当场启动新创建的线程！
        threads_[threadId]->start();
        
        // 5.5 同步递增总线程数与空闲线程数
        curThreadSize_++;
        idleThreadSize_++;
    }

    // 6. 将任务包装进 Result 对象返回
    return Result(sp);
}
```

### 2. 深度追问：扩容部分的四个关键设计细节

#### Q1：为什么扩容条件是 `taskSize_ > idleThreadSize_`，而不是 `taskSize_ > curThreadSize_`？
* `curThreadSize_` 代表线程池里的**总线程数**（包含正在耗时执行 `task->exec()` 的忙碌线程）。如果某个线程正在忙，它是无法立刻过来取新任务的。
* `taskSize_` 代表**当前还留在队列里没被拿走的任务数**（一旦某个线程从队列里 `pop()` 拿走任务，`taskSize_` 和 `idleThreadSize_` 都会同步减 $1$）。
* 因此，判断“人手够不够”的唯一标准是：**队列里积压的任务数（`taskSize_`）是否超过了当前能立刻干活的空闲线程数（`idleThreadSize_`）**。

#### Q2：为什么创建新线程时，除了 `curThreadSize_++`，还必须对 `idleThreadSize_++`？
很多初学者会疑惑：“新线程创建出来马上就要去干活了，为什么算作空闲线程？”
这必须结合工作线程入口函数 `threadFunc` 的**状态流转对称性**来看：
```cpp
// 新线程进入 threadFunc 后，一旦从队列取出任务，第一件事就是无条件执行：
idleThreadSize_--;
task = std::move(taskQue_.front());
taskQue_.pop();
taskSize_--;
```
* **生命周期闭环：** 在线程池的定义中，**任何线程在真正从队列中取走任务之前，它的初始身份都是 Idle（空闲）**。
* **防止下溢崩溃：** 如果新线程出生时不执行 `idleThreadSize_++` 垫底，当它进 `threadFunc` 抢到任务并执行 `idleThreadSize_--` 时，就会平白扣掉其他线程的空闲额度（若 `idleThreadSize_` 定义为无符号原子整型 `std::atomic_uint`，在 $0$ 的基础上减 $1$ 会直接下溢回绕为 `UINT_MAX`，导致后续扩容彻底瘫痪）。

#### Q3：为什么启动线程必须写 `threads_[threadId]->start()`，而不能写 `ptr->start()`？
因为 `ptr` 是 `std::unique_ptr<Thread>`（独占智能指针）。在上一行执行 `threads_.emplace(threadId, std::move(ptr));` 后，**`ptr` 内部管理的堆内存所有权已经被转移给哈希表，此时 `ptr` 本身变成了 `nullptr`**。若再调用 `ptr->start()` 会引发空指针异常，因此必须通过哈希表 `threads_[threadId]` 启动。

#### Q4：`threads_[threadId]->start()` 在 `idleThreadSize_++` 之前执行，会不会出现新线程先减 $1$、主线程后加 $1$ 的竞态条件？
**绝对不会。** 因为此时主线程在 `submitTask` 中正持有互斥锁 `taskQueMtx_`。新线程启动后进入 `threadFunc`，第一行代码就是申请同一把锁 `std::unique_lock<std::mutex> unique_lock(taskQueMtx_);`。在主线程执行完 `curThreadSize_++`、`idleThreadSize_++` 并离开 `submitTask` 作用域释放锁之前，新线程必然阻塞在锁门口，因此时序上是严格串行的。

---

## 四、 阶段二：空闲超时自动回收机制（`threadFunc` 源码剖析）

当新扩容的线程处理完突发任务后，如果后续没有新任务到来，线程池需要将超出 `initThreadSize_` 的多余线程自动销毁。这一过程完全由工作线程在 `threadFunc` 中自主完成。

### 1. 源码实现与逐行注释
```cpp
void ThreadPool::threadFunc(int threadId) {
    // 1. 记录当前线程刚启动（第一次进入空闲态）的时间戳
    auto lastTime = std::chrono::high_resolution_clock().now();

    for (;;) {
        std::shared_ptr<Task> task;
        {
            // 2. 获取任务队列互斥锁
            std::unique_lock<std::mutex> unique_lock(taskQueMtx_);

            std::cout << "tid: " << std::this_thread::get_id()
                      << " 尝试获取任务..." << std::endl;

            // 3. 只要任务队列为空（taskSize_ == 0），就循环等待
            while (taskSize_ == 0) {
                // 【配合析构函数】若线程池已停止运行，直接清理当前线程并退出
                if (!isPoolRunning_) {
                    threads_.erase(threadId);
                    exitCond_.notify_all();
                    return;
                }

                // 4. 【Cached 模式超时回收核心逻辑】
                if (poolMode_ == PoolMode::MODE_CACHED) {
                    // 4.1 采用 1 秒心跳轮询：每隔 1 秒钟超时醒来一次，检查累计空闲时长
                    if (std::cv_status::timeout == 
                        notEmpty.wait_for(unique_lock, std::chrono::seconds(1))) 
                    {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);

                        // 4.2 双重判定：连续空闲时间 >= 60s（或10s），且当前总线程数 > 初始核心线程数
                        if (dur.count() >= THREAD_MAX_IDLE_TIME
                            && curThreadSize_ > initThreadSize_) 
                        {
                            // 4.3 修改计数器，并将自身包装对象从哈希表中移除
                            curThreadSize_--;
                            idleThreadSize_--;
                            threads_.erase(threadId);

                            std::cout << "线程 " << threadId << " 空闲超时退出!" << std::endl;
                            return; // 4.4 直接 return 结束 threadFunc，unique_lock 自动解锁
                        }
                    }
                }
                else {
                    // Fixed 模式下：常驻核心线程直接无限期等待
                    notEmpty.wait(unique_lock);
                }
            }

            std::cout << "this thread tid: " << std::this_thread::get_id()
                      << " 获取任务成功..." << std::endl;

            // 5. 跳出 while 循环说明必定有任务（taskSize_ > 0）
            // 线程由空闲态转为忙碌态，取出队首任务
            idleThreadSize_--;
            task = std::move(taskQue_.front());
            taskQue_.pop();
            taskSize_--;

            // 6. 若队列中还有剩余任务，接力通知其他空闲线程继续消费
            if (!taskQue_.empty()) {
                notEmpty.notify_all();
            }

            // 7. 刚刚消费了一个任务，队列必定不满，通知 submitTask 可以继续生产
            notFull.notify_all();
        } // <--- 8. 退出局部作用域，自动释放 taskQueMtx_ 锁（千万不能带着锁执行耗时任务！）

        // 9. 在无锁状态下执行具体的任务逻辑
        if (task != nullptr) {
            task->exec();
        }

        // 10. 【关键】任务执行完毕，线程重新恢复为空闲状态，并刷新“最新开始空闲”的时间戳！
        idleThreadSize_++;
        lastTime = std::chrono::high_resolution_clock().now();
    }
}
```

### 2. 深度追问：超时回收部分的三个核心设计原因

#### Q1：为什么要记录 `lastTime` 并用 `wait_for(1秒)` 循环判断，而不是直接写 `wait_for(60秒)`？
如果直接写成 `notEmpty.wait_for(unique_lock, std::chrono::seconds(60))`，会导致**“陪跑唤醒重置计时”**的严重 Bug：
* 假设当前有 $5$ 个空闲线程都已经等了 $50$ 秒。此时用户调用 `submitTask` 提交了 **$1$ 个任务**，并执行了 `notEmpty.notify_all()`。
* 这 $5$ 个空闲线程会**全部被唤醒**去争抢锁：其中 $1$ 个线程抢到任务去执行，另外 **$4$ 个没抢到任务的“陪跑”线程**在下一轮 `while (taskSize_ == 0)` 检查时发现队列依然为空，只能再次进入等待。
* 如果用 `wait_for(60秒)`，这 $4$ 个陪跑线程再次调用 `wait_for(60秒)` 时，**之前已经苦等了 $50$ 秒的记录就彻底作废了，必须重新再等满 $60$ 秒**！只要每分钟零星来 $1$ 个任务，多余线程就永远无法回收。
* **采用 `lastTime` + `wait_for(1秒)` 的精妙之处：**
  `lastTime` **只有在真正抢到任务并执行完 `task->exec()` 之后（第 10 步）才会被刷新**。就算线程中途被 `notify_all()` 虚假/陪跑唤醒，它的 `lastTime` 依然纹丝不动。每隔 $1$ 秒超时醒来计算一次 `now - lastTime`，能够极其精准地衡量该线程“真正没干活的连续空闲时间”。

#### Q2：超时退出时，为什么 `curThreadSize_--` 和 `idleThreadSize_--` 都要减 $1$？
因为能够停留在 `while (taskSize_ == 0)` 循环内部的线程，要么是刚启动还没抢到过任务，要么是上一轮任务执行完毕后已经执行了第 10 步的 `idleThreadSize_++`。此时它的身份就是**空闲线程**。当它超时退出时，线程池不仅少了一个总线程（`curThreadSize_--`），也少了一个可用的空闲线程（`idleThreadSize_--`）。

#### Q3：线程在运行中调用 `threads_.erase(threadId)` 把代表自己的 `Thread` 对象销毁了，为什么不会崩溃？
初学者常担心：“我把我自己 `delete` 了，后面紧接着执行 `return` 难道不会内存非法访问吗？”
这里需要区分三个完全不同层面的概念：

1. **底层操作系统线程（`std::thread`）：** 在 `Thread::start()` 中创建后当场调用了 `t.detach()`。这意味着底层内核线程已经与 C++ 对象解绑。销毁包装类 `Thread` 不会终止正在运行的内核线程，只有当 `threadFunc` 执行完 `return;` 函数栈弹出后，操作系统内核才会自动回收该线程栈。
2. **包装类对象（`class Thread`）：** 存储在 `threads_` 哈希表的 `std::unique_ptr<Thread>` 中，里面仅仅保存了一个 `func_` 副本和一个 `int threadId_`。调用 `threads_.erase(threadId)` 释放的只是堆上这个 `Thread` 小壳子的内存。
3. **当前正在执行的函数上下文（`ThreadPool::threadFunc`）：** 它是 **`ThreadPool` 类的成员函数**，隐式 `this` 指针指向的是 `ThreadPool` 对象，而 `threadId` 是当前函数栈上的**按值传递形参**。在 `threads_.erase(threadId)` 之后，代码既没有访问 `Thread` 类的任何成员，也没有再使用失效迭代器，因此绝对安全。

---

## 五、 实战避坑指南：`Cached` 模式调试中最常见的 3 个 Bug

在自己手写测试 `Cached` 模式时，以下三个问题出现频率极高：

### 1. 现象：无论提交多少个任务，控制台始终不创建新线程
* **原因排查：**
  检查测试代码中是否调用了 `threadPool.setThreadSizeThreshHold(4);` 将最大线程上限设为了 `4`，而 `threadPool.start();` 默认参数 `initThreadSize` 也是 `4`。
  这会导致扩容条件 `curThreadSize_ < threadSizeThreshHold_` 变成 `4 < 4`（恒为 `false`）。
* **修复方法：** 确保设置的 `threadSizeThreshHold_` **严格大于** `initThreadSize_`（例如初始 `4` 个线程，上限设为 `8` 或 `10`）。

### 2. 现象：运行 1 秒后直接崩溃在 `<deque>` 的 `"front() called on empty deque"`
* **原因排查 A（循环写成分支）：**
  在 `threadFunc` 中，如果把 `while (taskSize_ == 0)` 误写成了 **`if (taskSize_ == 0)`**，那么 `Cached` 模式下 `wait_for(1秒)` 第 $1$ 秒超时返回时（此时还没满 $60$ 秒，不走 `return`），代码会直接走出 `if` 块去执行 `taskQue_.front()`，而此时队列为空，当场触发 STL 断言崩溃。
* **原因排查 B（主线程作用域提前结束）：**
  在 `main()` 函数中，如果只调用了第 1 个任务的 `res.get()` 就退出了 `try {}` 作用域（而 `getchar()` 写在了 `try` 块外面），且此时尚未实现 `~ThreadPool()` 的阻塞等待回收逻辑，那么主线程会提前析构 `threadPool` 对象并将 `taskQue_` 清空，后台还在运行的 `detach` 子线程再去访问 `taskQue_.front()` 就会立刻崩溃。

### 3. 现象：如果同一个进程创建了第二个 `ThreadPool` 对象，`start()` 时发生空指针崩溃
* **原因排查：**
  在 `ThreadPool::start()` 启动初始线程时，如果写成按下标 `[i]` 访问：
  ```cpp
  // ❌ 隐患写法：
  for (int i = 0; i < initThreadSize_; ++i) {
      threads_[i]->start(); // 若创建第 2 个线程池，generateId_ 已从 4 开始累加，threads_[0] 为空指针！
      idleThreadSize_++;
  }
  ```
* **修复方法：** 直接遍历哈希表启动，或者在上面的创建循环中用局部数组记录生成的 `threadId`：
  ```cpp
  // ✅ 安全写法：直接遍历 unordered_map 启动所有初始线程
  for (auto& pair : threads_) {
      pair.second->start();
      idleThreadSize_++;
  }
  ```