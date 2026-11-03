# 线程池死锁与并发 Bug 排查笔记

> 基于本项目当前 `Task / Result / Semaphore / ThreadPool` 实现整理。
>
> 审查日期：2026-11-02。本文记录问题分析、此前的本地复现结果和修复思路；文中的改造示例不是已合入实现的补丁。

## 1. 本次检查的结论

当前线程池的普通执行路径可以正常工作：创建一个池，提交一个任务，保存 `Result`，调用一次 `get()`，最后析构。

现有测试中的约 5 秒停顿，来自 `MyClass::run()` 内的 `sleep_for()`。此前本地运行该测试，能够返回 `3` 并完成线程池析构。

但是，接口的边界条件、结果对象的生命周期、任务之间的依赖和启动失败处理存在问题。

| 场景 | 问题类型 | 主要后果 | 本次证据 |
| --- | --- | --- | --- |
| 工作线程等待同池子任务，且执行线程全部被占用 | 执行资源死锁 | 子任务不能开始，父任务不能结束 | 已复现并检查调用栈 |
| 同一个 `Result` 连续调用两次 `get()` | 永久等待 | 第二次没有新的完成信号 | 已复现 |
| 同一个 `Task` 对象重复提交 | 结果关联被覆盖 | 较早的 `Result` 收不到通知 | 已复现 |
| 未启动就提交任务 | 永久等待 | 返回有效结果，但没有执行者 | 已复现 |
| 固定模式使用 `start(0)` | 永久等待 | 队列有任务，没有工作线程 | 已复现 |
| 工作线程销毁自己的线程池 | 自等待死锁 | 析构等待当前工作线程退出 | 已复现 |
| `Result` 在任务结束前销毁 | 生命周期错误、未定义行为 | 悬空指针、崩溃或假死 | 静态分析确认风险，未运行内存检测器验证 |
| 启动线程期间抛异常 | 异常安全缺陷 | 析构可能永久等待未启动的条目 | 静态分析，未实际耗尽系统资源 |
| 第二个线程池启动 | 空指针访问 | 启动时崩溃 | GDB 捕获 `SIGSEGV` |
| `run()` 的异常逃出线程入口 | 未捕获异常 | `std::terminate` | 已观察到终止输出 |
| cached 模式初始线程数为 0，最后一个扩容线程超时退出 | 回收边界问题 | 可能遗留任务或缺少退出通知 | 静态推导，未动态复现 |
| 提交空 `shared_ptr<Task>` | 空指针访问 | 构造 `Result` 时解引用空指针 | 静态分析 |

**证据解释：测试超时只能证明给定时间内没有退出。永久等待或死锁的判断，还需要结合代码和等待依赖关系。**

## 2. 先区分几种容易混淆的现象

| 现象 | 含义 | 本项目中的例子 |
| --- | --- | --- |
| 正常阻塞 | 等待的事件未来能够发生 | 主线程等待一个耗时 5 秒的任务 |
| 死锁 | 等待形成闭环，相关执行者无法继续 | 唯一 worker 执行父任务，父任务等待同池子任务 |
| 永久等待 | 条件没有后续完成者，不一定存在闭环 | 第二次 `get()` 等待已经不会再次发布的信号 |
| 饥饿 | 有执行机会，但某个任务长期得不到资源 | 某类任务持续被其他任务挤占 |
| 活锁 | 线程持续活动，却无法完成有效工作 | 两个参与者反复退让、重试 |
| 数据竞争 | 不同线程缺少同步排序地访问同一内存位置，至少一方写入，且至少一方是非原子访问 | 重复并发提交时读写同一个 `Task::result_` |
| 生命周期错误 | 访问了已销毁的对象 | 任务回写已经析构的 `Result` |
| 进程终止 | 程序已经出错或进入终止流程 | 工作线程的异常未捕获 |

CPU 很低、控制台没有输出、调用栈停在 `wait()`，都不足以单独证明死锁。

排查的核心问题是：

> 当前线程在等什么？谁能满足这个条件？那个执行者现在又在等什么？

## 3. 理解当前实现的执行链路

### 3.1 提交与执行

```text
调用线程
  └─ submitTask(task)
       ├─ 获取 taskQueMtx_
       ├─ 等待队列有空间
       ├─ 任务入队
       ├─ 通知 notEmpty
       ├─ 构造 Result，绑定 Task::result_
       └─ 释放队列锁，返回 Result

工作线程
  └─ threadFunc()
       ├─ 获取 taskQueMtx_
       ├─ 等待任务或关闭状态
       ├─ 任务出队
       ├─ 通知 notFull
       ├─ 释放队列锁
       └─ Task::exec()
            ├─ run()
            └─ Result::setValue()
                 └─ Semaphore::post()

结果读取线程
  └─ Result::get()
       ├─ Semaphore::wait()
       └─ 移出 Any
```

### 3.2 锁与条件变量的职责

| 对象 | 保护或协调的内容 | 等待条件与促成方 |
| --- | --- | --- |
| `taskQueMtx_` | 任务队列，以及常规运行路径中的线程条目管理 | 提交、出队、退出操作共享此锁；当前 `start()` 另有生命周期约束和缺陷 |
| `notEmpty` | 工作线程等待任务 | 提交方使队列非空；关闭方通知线程退出 |
| `notFull` | 提交方等待队列空间 | 工作线程出队后释放空间 |
| `exitCond_` | 析构等待工作线程退出 | 工作线程删除自己的条目并通知 |
| `Semaphore::mtx_` | 信号量计数 `resLimit_` | `post()` 增加计数，`wait()` 消耗计数 |
| `Semaphore::cond_` | 结果尚未完成时阻塞读取者 | 对应任务调用 `setValue()` 后发布资源 |

两个重要观察：

1. 当前任务的 `exec()` 在队列锁之外执行，这个安排是正确的。
2. `atomic` 只保证相应变量操作的原子性，不会自动保证整个队列协议、生命周期或任务依赖正确。

## 4. 问题一：同池嵌套等待导致执行资源死锁

### 4.1 最小触发条件

1. 固定线程池只有一个 worker。
2. worker 正在执行父任务。
3. 父任务向同一个线程池提交子任务。
4. 父任务调用子任务的 `get()`。

示意代码如下，假设 `ParentTask` 持有线程池引用，`ChildTask` 是一个正常返回结果的任务：

```cpp
Any ParentTask::run()
{
    Result child = pool_.submitTask(std::make_shared<ChildTask>());
    return child.get();
}
```

### 4.2 等待关系

```mermaid
flowchart LR
    W["唯一工作线程：执行父任务"] -->|等待| R["子任务完成"]
    R -->|需要| C["执行队列中的子任务"]
    C -->|需要工作线程空闲| W
```

父任务等待子任务，子任务等待 worker，worker 被父任务占用。

这是执行资源形成的循环等待，并不要求出现两把 mutex 的锁顺序冲突。

### 4.3 为什么增加线程数不能保证解决

线程数从 1 改为 N 后，如果 N 个 worker 都执行了等待子任务的父任务，所有子任务仍然只能排队。

cached 模式也只能缓解：达到线程上限，或者扩容不足以覆盖依赖时，同样可能耗尽执行资源。

### 4.4 修复方向

- 由池外线程提交并协调多个任务的结果。
- 把“等待后继续执行”的逻辑拆成后续任务，使父任务先释放 worker。
- 必要时使用不同执行器，但仍要检查不同执行器之间是否形成循环依赖。
- 调度器可以设计等待期间帮助执行任务，但需要处理重入、递归深度、锁依赖和任务顺序等问题。

**换成 `std::future` 只改变结果传递机制，不会自动打破任务依赖。**

定位入口：`ThreadPool.cpp` 中的 `threadFunc()`、`Task::exec()`、`Result::get()`。

## 5. 问题二：重复 get() 没有第二个信号

当前实现的核心逻辑：

```cpp
void Result::setValue(Any any) {
    any_ = std::move(any);
    sem_.post();
}

Any Result::get() {
    if (!isValid_) {
        return "";
    }
    sem_.wait();
    return std::move(any_);
}
```

正常提交且只执行一次的任务，对应状态为：

```text
初始计数           0
任务完成 post()   1
第一次 get()      0，正常返回
第二次 get()      等待计数 > 0，但没有第二个 post()
```

复现：

```cpp
Result r = pool.submitTask(std::make_shared<MyClass>());
r.get();
r.get();  // 永久等待
```

修复前需要先定义接口语义：

| 期望语义 | 设计方式 |
| --- | --- |
| 结果只能取一次 | 记录 `consumed`，重复读取明确报错 |
| 结果允许多次读取 | 使用持续的 `ready` 状态，并设计返回值共享或复制语义 |

当前 `Any` 只能移动，适合一次性取值。不能只去掉第二次的信号量等待，因为结果在第一次读取时已经被移走。

## 6. 问题三：重复提交同一个 Task 覆盖结果关联

### 6.1 原因

`Task` 只保存一个结果指针：

```cpp
Result* result_;
```

每次构造 Result 都重新绑定：

```cpp
Result::Result(std::shared_ptr<Task> task, bool isValid)
    : task_(task), isValid_(isValid)
{
    task_->setResult(this);
}
```

如果执行之前连续提交同一个对象：

```cpp
auto task = std::make_shared<MyClass>();
Result first = pool.submitTask(task);
Result second = pool.submitTask(task);
```

则关联被依次改为：

```text
task.result_ = &first
task.result_ = &second
```

任务队列中的两个条目指向同一个 Task 对象，执行时都可能把结果写到 `second`，`first` 永远收不到通知。

### 6.2 如何稳定复现

不要依赖工作线程“恰好慢一点”。可以用信号量固定顺序：

1. 创建单线程池。
2. 提交 GateTask，让唯一 worker 阻塞在自定义信号量上。
3. 连续两次提交同一个 Task，分别保存 Result。
4. 打开 GateTask 的信号量。
5. 两次执行都完成，但第一个 Result 仍无法返回。

这个复现不需要依赖数据竞争：覆盖在工作线程访问该对象之前就已发生。

### 6.3 相关变体

- 同一 Task 并发重复提交，还可能产生 `result_` 的读写竞争和结果值的并发写入。
- 提交失败返回 `Result(sp, false)` 时，构造函数仍然会修改 Task 绑定，也可能覆盖先前成功提交的结果通道。

### 6.4 修复方向

可以明确拒绝 Task 重复提交，也可以让每次提交拥有独立执行记录：

```text
Submission A = Task + State A
Submission B = Task + State B
```

工作线程从队列取出本次提交的记录，直接向该记录的状态交付结果，不再从 `Task::result_` 查找结果对象。

即使结果状态已独立，同一个业务 Task 是否允许并发执行，仍然取决于其 `run()` 是否线程安全。

## 7. 问题四：Result 提前析构造成悬空指针

### 7.1 危险用法

```cpp
pool.submitTask(std::make_shared<SlowTask>());
```

临时 Result 在分号处销毁。如果任务尚未完成，它保存的 `Result*` 就已经悬空。

另一个例子：

```cpp
{
    Result r = pool.submitTask(std::make_shared<SlowTask>());
} // r离开作用域，任务可能还没有结束
```

随后任务执行：

```cpp
result_->setValue(run());
```

就可能访问已经销毁的 `Any`、mutex 或 condition variable。

### 7.2 为什么 shared_ptr<Task> 没有解决问题

当前所有权方向是：

```text
Result ── shared_ptr ──→ Task
Task   ── 裸指针 ─────→ Result
```

Result 可以延长 Task 的生命，但 Task 没有延长 Result 的生命。

队列中的 `shared_ptr<Task>` 保证 Task 活着，不代表 Task 内部保存的所有指针都有效。

### 7.3 正确的共享状态方向

```text
本次提交的执行记录 ── shared_ptr ──→ SharedState
Result             ── shared_ptr ──→ SharedState
```

共享状态保存值、异常和完成标志。调用者丢弃 Result 后，执行记录仍然持有状态，任务可以安全完成交付。

**这是未定义行为问题。具体表现不确定，不能承诺它一定卡死或一定崩溃。**

仅仅把裸指针换成原子指针，或者在析构里把它清空，也没有解决并发回写与对象销毁之间的生命周期竞争。

## 8. 问题五：工作线程销毁自己的线程池

当前析构等待：

```cpp
exitCond_.wait(lock, [this] {
    return threads_.empty();
});
```

若某个任务销毁所属线程池：

```text
当前worker进入线程池析构
  → 析构等待所有worker退出，包含当前worker
  → 当前worker需要等析构返回才能完成任务
  → 当前worker无法进入自己的退出路径
```

这是自等待。把 `detach()` 改为 `join()` 也不会自动解决“当前线程等待自己”的设计问题。

应由外部线程管理线程池的阻塞关闭和销毁，明确禁止所属 worker 销毁线程池。不能通过让析构直接返回、销毁仍在使用的池状态来规避等待。

## 9. 调用方业务锁也可能参与死锁

即使任务执行时没有持有队列锁，调用方仍可能引入循环依赖。

示意代码，假设 `LockTask::run()` 需要获取 `businessMutex`：

```cpp
auto pool = std::make_unique<ThreadPool>();
pool->start(1);

std::mutex businessMutex;
std::unique_lock<std::mutex> held(businessMutex);

Result result = pool->submitTask(
    std::make_shared<LockTask>(businessMutex));

pool.reset();  // 持有业务锁时，等待任务和worker退出
```

依赖为：

```text
主线程：持有 M → 析构池 → 等worker退出
worker：执行任务 → 等 M → 无法完成
```

持有业务锁时调用 `result.get()`，而任务也需要同一把业务锁，同样可能死锁。

排查时需要沿完整调用栈检查调用者持有的锁；主线程停在池析构里，不代表根因一定在池析构的条件变量。

## 10. 启动、回收和异常处理中的其他缺陷

### 10.1 未启动或零线程时仍接受任务

当前 `submitTask()` 没有检查运行状态，`start()` 也不校验线程数。

未启动就提交，或固定模式 `start(0)` 后提交，都可能得到有效 Result，却没有执行者。负数线程数同样应被拒绝。

默认参数 `std::thread::hardware_concurrency()` 只是硬件并发度提示，标准允许返回 0。

修复思路：默认值至少取 1；显式传入非法值时清晰报错；提交时检查池状态，并明确拒绝原因。

### 10.2 第二个池的线程 ID 不再从 0 开始

创建时使用全局递增 ID：

```cpp
threadId_(generateId_++)
```

启动时却按整数索引访问：

```cpp
for (int i = 0; i < initThreadSize_; ++i) {
    threads_[i]->start();
}
```

`unordered_map` 的键不等于数组下标。第二个池的真实 ID 可能从 1、4 或其他值开始，访问不存在的键会插入空 `unique_ptr`，随后解引用。

调试器实际捕获的关键栈为：

```text
ThreadPool::start(...):132
Thread::start(this=0x0):227
SIGSEGV
```

应根据实际插入的 ID 或实际条目启动线程。多个池并发创建线程时，还需要同步全局 ID 生成。

### 10.3 启动异常没有回滚

当前顺序是：

```text
先向threads_插入全部Thread对象
再逐个创建系统线程
```

如果系统线程创建失败，可能留下“有条目、没有运行线程”的对象。栈展开进入池析构后，析构等待条目被工作线程删除，但未启动的条目根本没有对应执行者。

正确的失败处理需要：

1. 区分已创建的管理对象和真正启动的工作线程。
2. 启动失败时停止继续接受任务。
3. 唤醒并回收已经启动的线程。
4. 清理未启动的条目。
5. 再把启动错误交还调用者。

本次没有通过耗尽系统资源复现此问题，结论来自启动及析构路径分析。

### 10.4 cached 超时回收缺少重新检查

当前 `wait_for()` 超时后，直接根据空闲时长和线程数量决定退出，没有再次检查队列与关闭状态；该回收分支也没有通知 `exitCond_`。

当允许初始线程数为 0 时，最后一个扩容线程可能沿该分支退出。

一种遗留任务的交错是：

```text
worker等待超时，准备重新取得队列锁
提交者先取得锁并放入新任务
worker随后取得锁，仍按超时分支退出
队列有任务，但已经没有worker
```

一种析构缺少通知的交错是：

```text
最后一个扩容worker已进入超时返回路径
析构取得锁，通知关闭，然后等待threads_为空
worker取得锁，沿超时分支删除最后条目
该分支没有通知exitCond_，析构可能继续等待
```

**边界限制：初始线程数至少为 1 时，`curThreadSize_ > initThreadSize_` 会阻止最后一个线程沿此分支退出。不能据此认定普通 cached 关闭必定死锁。**

改造方向是重新检查任务和关闭状态，并统一工作线程的退出统计、条目删除和退出通知。衡量空闲时长宜使用单调的 `steady_clock`。

### 10.5 任务异常逃出工作线程

当前 `run()`、`exec()` 和工作线程入口之间没有异常捕获。异常逃出线程入口时，会触发 `std::terminate`；主线程的 `try/catch` 不能跨线程捕获它。

修复应在任务执行包装层捕获异常，保存 `exception_ptr`，标记结果完成并通知读取者，由 `get()` 重新抛出。

只加 `catch` 打印日志、却不发布失败状态，会把当前的进程终止改成结果永久等待。

### 10.6 编译与接口细节

- 头文件应直接包含其使用的 `<thread>`、`<condition_variable>` 和 `<stdexcept>` 等头文件，不应依赖其他头文件间接包含。
- 本次 GCC 原样编译发现了缺少头文件的问题。为保持实现不变，复现使用强制包含的 `prelude.h` 补齐依赖。
- 使用当前不可复制的 Result 返回方式时，应保持相关构建配置使用 C++17 或更高版本；不要只检查 Debug x64 配置。
- 提交失败用空字符串包装成 Any，调用者再转成目标类型时才报类型错误，不利于区分“提交失败”和“任务失败”。应设计明确的错误通道。
- `notFull.wait_for(..., 1s)` 不能保证整个 `submitTask()` 一秒内返回：此前获取队列锁、等待后重新获取锁，以及其他函数工作都可能增加耗时。
- `start()` 应明确只能执行一次，配置与启动的并发调用也需要有清晰约束。

## 11. 当前实现中容易误报的地方

### 11.1 持 unique_lock 调用 wait，不表示一直持锁睡眠

```cpp
std::unique_lock<std::mutex> lock(taskQueMtx_);
exitCond_.wait(lock, [this] { return threads_.empty(); });
```

若谓词为假，等待过程会原子地释放锁并进入等待；唤醒后重新获取锁，再检查谓词。

可理解为：

```cpp
while (!predicate()) {
    cv.wait(lock);  // 等待期间释放lock，返回前重新加锁
}
```

所以当前析构等待期间，worker 能取得队列锁执行退出。

析构虽然在取得锁前修改运行标志，但它在取得同一队列锁后才通知；worker 的检查和进入等待也受该锁协调。当前这条路径不能直接判为丢唤醒。把关闭状态统一放到锁内修改，有利于维护协议，但属于改进建议。

### 11.2 通知发生在 Result 构造前，不代表 worker 会提前执行

`submitTask()` 一直持有队列锁。返回对象构造完成后，局部锁才销毁。worker 必须取得同一把锁才能出队，因此普通提交中可以先完成 Result 绑定，再开始执行。

这不代表重复提交同一 Task 也安全；重复提交的问题是后续绑定覆盖先前绑定。

### 11.3 先 post 后 wait 不会丢资源

Semaphore 保存的是受锁保护的计数。先 `post()` 把计数改为 1，后来进入 `wait()` 的线程仍能观察并消费它。

条件变量通知本身不保存“历史通知次数”。这里保存状态的是 `resLimit_`，不能把通知与资源计数混为一谈。

### 11.4 单次完成与单次读取有同步关系

正常单次结果交付中：

```text
worker写any_
  → post()获取并释放信号量锁
  → get()在同一把锁下观察到可消费资源
  → 读取any_
```

这条路径不能直接报成 `any_` 必然存在数据竞争。多个发布者、重复 Task、多个消费者和生命周期错误需要另外分析。

### 11.5 C++17 返回路径有保证拷贝消除

```cpp
return Result(sp);
```

在 C++17 对应的返回与初始化路径中，结果可直接构造在调用者的目标位置。不能笼统认为函数里必然产生一个随后销毁的 Result，再把它的地址留给 Task。

真正需要修复的是调用者丢弃返回值或提前销毁结果时的生命周期。

### 11.6 当前没有持队列锁析构 Task 的明显重入问题

取出任务时：

```cpp
task = std::move(taskQue_.front());
taskQue_.pop();
```

队列中的 `shared_ptr` 已被移动为空，`pop()` 不会在这里销毁被取出的 Task。局部 `task` 在每轮循环结束时销毁，届时队列锁已释放。

当前 `Thread` 析构也为空，不能仅凭持锁 `threads_.erase()` 就推断存在业务回调重入。

## 12. Visual Studio 排查步骤

### 第一步：保留现场，暂停全部线程

使用带调试信息的构建，例如 Debug x64。发现卡住时，选择“调试 → 全部中断”。

打开：

- “调试 → 窗口 → 线程”。
- “调试 → 窗口 → 并行堆栈”。
- “调试 → 窗口 → 调用堆栈”。

如果只是盯着主线程，很容易只看到 `get()` 或析构，错过真正阻塞的工作线程。

### 第二步：逐个线程找到自己的代码

底层栈顶可能是操作系统等待函数。沿栈向调用者方向找到自己的函数，并记录：

```text
线程身份
当前Task
等待位置
等待条件
持有的业务锁或执行资源
预期通知者
```

不要把运行库的辅助线程全部当成线程池 worker；结合 `threadFunc()` 调用栈识别自己的工作线程。

### 第三步：检查状态

在对应池或 Result 的栈帧下检查：

```text
taskSize_
curThreadSize_
idleThreadSize_
isPoolRunning_
taskQue_
threads_
对应Result的resLimit_
```

注意区分线程池自定义的 `threadId_` 与 `std::this_thread::get_id()`，二者不是同一种编号。

| 状态与堆栈特征 | 优先调查方向 |
| --- | --- |
| 队列有任务，所有 worker 都在结果等待中 | 同池嵌套依赖 |
| Task 已执行两次，但第一个 Result 未完成 | 重复提交覆盖绑定 |
| 一次读取成功后，第二次停在信号量 | 重复 get |
| 主线程等析构，worker 等业务 mutex | 调用方持锁关闭 |
| `threads_` 有条目，但找不到对应运行线程 | 启动失败或线程管理错误 |
| 空队列，空闲 worker 在 `notEmpty` | 通常是正常空闲，继续看主线程目标 |
| 调试器捕获空指针或终止异常 | 先按崩溃与异常路径排查 |

### 第四步：画等待关系

为每个阻塞者写一句：

```text
A等待B完成条件；B要继续，又需要C释放资源。
```

持续追踪，直到遇到以下结果之一：

- 回到起点：形成循环依赖。
- 没有通知者或执行者：永久等待。
- 执行者正在有效推进：可能是正常耗时。
- 对象已经销毁：先处理生命周期与内存错误。

### 第五步：检查条件变量的完整协议

对每个 `wait()` 核查：

1. 等待谓词是什么？
2. 状态由谁修改？
3. 修改与检查如何同步？
4. 修改状态后由谁通知？
5. 正常、失败、关闭、取消路径是否都能到达终态？
6. 条件变量和互斥锁是否能活到等待结束？

条件变量可能虚假唤醒，因此必须循环检查条件。单纯补一个 `notify_all()` 不能让不存在的结果变成真实结果。

### 第六步：最小化并固定交错

- 把线程数降到 1，观察是否出现嵌套依赖。
- 用 gate、信号量或屏障固定步骤顺序。
- 让每个复现只验证一个问题，避免内存错误干扰逻辑分析。
- 卡住的测试放在独立进程中，由外部脚本设置超时。
- 修复后验证该场景能够完成，并断言正确的结果或错误状态。

多次增加 `sleep()` 只能改变概率，不能证明顺序；优先让测试明确控制“哪个事件先发生”。

## 13. 日志和工具应该怎样配合

### 13.1 日志记录状态转移

比起只打印“尝试获取任务”，更有用的字段是：

```text
时间戳 | OS线程ID | poolID | submissionID | task地址 | state地址
事件 | 队列长度 | worker数量 | 空闲数量 | 池状态
```

建议事件包括：

```text
submit.begin / submit.accept / submit.reject
task.dequeue / task.begin / task.success / task.failure
result.wait / result.ready / result.consume
pool.stop / worker.exit
```

日志需要能区分同一个 Task 的不同提交次数，以及结果实际交付给了哪个状态。

输出日志也会改变调度与耗时。尽量缩短队列锁内的工作，不要只依赖某一种日志输出顺序判断线程先后。

### 13.2 工具与问题类型对应

| 问题 | 合适的手段 | 能力边界 |
| --- | --- | --- |
| 逻辑死锁、任务依赖死锁 | 全线程调用栈、状态快照、等待图 | 需要理解通知者和资源关系 |
| 悬空指针、越界 | AddressSanitizer | 不是所有逻辑卡住的检测器 |
| 数据竞争 | ThreadSanitizer | 需使用受支持的平台和构建，检测不到不代表没有逻辑死锁 |
| 偶发调度问题 | 可控同步复现、独立进程超时、有限压力测试 | 超时只说明没有按时完成 |

MSVC 可以使用 `/fsanitize=address` 构建后运行内存错误复现；启用时按工具文档处理不兼容的构建选项。Linux 等受支持平台可以使用 Clang 的 `-fsanitize=thread -g -O1` 检查数据竞争。

## 14. 修复设计：结果状态与每次提交绑定

下面是设计示意，省略了完整类声明、接口适配和调度逻辑，不能直接替换当前几个函数。

### 14.1 独立状态和提交记录

```cpp
struct SharedState {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    bool consumed = false;
    Any value;
    std::exception_ptr error;
};

struct Submission {
    std::shared_ptr<Task> task;
    std::shared_ptr<SharedState> state;
};
```

每次成功提交创建一个状态，Result 和该次执行记录分别持有它。提交失败不要修改其他提交的绑定。

### 14.2 执行端完成值或异常

```cpp
Any value;
std::exception_ptr error;

try {
    value = submission.task->run();
} catch (...) {
    error = std::current_exception();
}

{
    std::lock_guard<std::mutex> lock(submission.state->mutex);
    submission.state->value = std::move(value);
    submission.state->error = error;
    submission.state->ready = true;
}
submission.state->cv.notify_all();
```

这里的设计约束是：每次提交只有一个结果发布者；执行失败同样进入可观察的完成状态。

### 14.3 读取端明确一次性消费

```cpp
Any Result::get()
{
    auto state = state_;  // Result持有的shared_ptr<SharedState>
    if (!state) {
        throw std::logic_error("invalid result");
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&] { return state->ready; });

    if (state->consumed) {
        throw std::logic_error("result already consumed");
    }
    state->consumed = true;

    if (state->error) {
        std::rethrow_exception(state->error);
    }
    return std::move(state->value);
}
```

`ready` 是持续成立的完成状态，`consumed` 表示是否已被读取。把“完成”和“消费”拆开，就不必靠一次信号量资源同时表达两件事。

也可以使用 `std::promise / std::future / std::packaged_task` 实现标准的共享结果与异常传递机制。任务之间如何等待，仍需独立设计。

## 15. 修复设计：启动与关闭协议

建议明确状态转换，例如：

```text
Created → Running → Stopping → Stopped
```

还要定义启动失败如何清理，并区分对象生命周期和可并发调用的操作。

推荐的关闭职责：

1. 在状态协议下停止接受新任务。
2. 明确排队任务是继续执行还是取消。
3. 唤醒等待任务的 worker；如果允许并发提交与显式关闭，也唤醒等待空间的提交方，使其观察停止状态。
4. 取消的任务必须向结果状态发布取消或失败，不能只丢掉队列条目。
5. 在不持队列锁的情况下等待真正启动的线程退出。
6. 清理线程对象与共享资源。

这不代表添加状态判断后，就允许任意线程在其他线程仍调用成员函数时销毁池对象。调用者仍需要保证池对象的生命周期覆盖这些调用。

### 15.1 join 改造的两个陷阱

如果池保存可 `join()` 的线程，不能持队列锁执行 join：worker 退出时也可能需要同一把锁。

也不能保留“worker 删除自身 joinable 线程对象”的结构。销毁仍 joinable 的 `std::thread` 会终止进程；自 join 也不是合法的关闭方案。cached worker 应先报告退出，再由其他线程完成 join 和清理。

当前 detach 加退出握手的正常路径，不能直接认定必然发生对象越界使用；改成 join 的目的是让生命周期和失败处理更清楚，必须配套改造回收方式。

### 15.2 超时为什么不是根本修复

给 `get()` 加超时可以帮助诊断，但不会自动执行子任务、打破业务锁依赖或取消任务。

当前裸指针设计中，如果超时返回后 Result 被销毁，worker 后续回写还可能访问悬空对象。因此应先解决共享状态生命周期，再设计超时与取消语义。

## 16. 本地复现记录与复用方式

此前在 Windows / MSYS2 UCRT64 GCC 16.1.0、C++17 构建下进行了复现。为保持实现不变，通过强制包含补齐缺少的头文件。

本次工作区的临时文件位于：

```text
out/deadlock-audit/prelude.h
out/deadlock-audit/repro.cpp
out/deadlock-audit/run_repros.py
```

`out` 被 `.gitignore` 忽略，这些文件属于本次诊断产物，重新克隆仓库时不保证存在。本文前面的原因分析与复现步骤不依赖这些文件长期保存。

在项目根目录，且本机具有相同 GCC 安装路径时，可以运行：

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH

g++ -std=c++17 -pthread -g -O0 -I ThreadPool `
    -include out/deadlock-audit/prelude.h `
    ThreadPool/ThreadPool.cpp out/deadlock-audit/repro.cpp `
    -o out/deadlock-audit/repro.exe

python out/deadlock-audit/run_repros.py repro.exe `
    normal nested double_get zero not_started repeat_task self_destroy
```

脚本为每个测试启动独立进程，3 秒超时后终止对应测试进程。上面选择的 `normal` 是快速返回的诊断任务，并非现有休眠 5 秒的演示程序。

| 模式 | 此前观察到的结果 | 分类依据 |
| --- | --- | --- |
| `normal` | 正常返回 42，退出码 0 | 普通路径完成 |
| `nested` | 输出父任务等待子任务后超时 | 调用栈和执行资源闭环 |
| `double_get` | 第一次读取成功，第二次超时 | 不存在第二次发布 |
| `zero` | 超时 | 固定模式没有 worker |
| `not_started` | 超时 | 没有启动执行者 |
| `repeat_task` | 任务执行两次，第一个结果仍超时 | 结果关联被覆盖 |
| `self_destroy` | worker进入析构后超时 | 当前worker等待自己退出 |
| `two_pools` | GDB捕获 `Thread::start(this=0x0)` | 空指针崩溃 |
| `throws` | 输出 `terminate called...` | 未捕获线程异常 |

Windows 的异常报告或终止界面可能使崩溃场景在脚本里也表现成超时。因此不要直接把 `two_pools`、`throws` 的 TIMEOUT 当作死锁。

第二个池的崩溃可使用以下方式检查：

```powershell
gdb -q -batch -ex run -ex 'thread apply all bt' `
    --args out/deadlock-audit/repro.exe two_pools
```

## 17. 建议的修复顺序与回归检查

### 17.1 修复顺序

1. 让每次提交拥有独立共享结果状态，消除 Result 裸指针回写。
2. 明确一次性取值和 Task 重复提交策略，覆盖提交失败路径。
3. 捕获任务异常，向读取者交付失败状态。
4. 修复线程 ID 索引、非法线程数、未启动提交和重复启动。
5. 补齐启动失败回滚、cached 回收和关闭协议。
6. 明确同池同步等待、业务锁和池内销毁的调用约束。

### 17.2 回归检查清单

- [ ] 普通任务正确返回结果并正常关闭。
- [ ] 任务抛异常后，读取方获得异常，其他任务仍能运行。
- [ ] 丢弃 Result 或提前结束其作用域，不影响任务安全完成。
- [ ] 第二次读取按定义报错或返回共享值，不永久等待。
- [ ] 重复提交 Task 被明确拒绝，或各次结果独立。
- [ ] 提交失败不会修改先前提交的结果状态。
- [ ] 连续创建两个池可以正常运行；若支持并发创建，也覆盖该场景。
- [ ] 非法线程数、空任务、未启动提交得到明确处理。
- [ ] 启动失败能回收已启动线程，不等待不存在的执行者。
- [ ] 固定模式、cached扩容与空闲回收都能正常关闭。
- [ ] 每一种被接受的提交最终进入成功、失败或取消状态。
- [ ] 调用约定或调度设计能够处理同池等待及池内关闭。

## 18. 自查问答

**问：主线程停在 `get()`，说明信号量实现错了吗？**

不一定。先找对应任务的位置，再检查它是否执行、是否发布到了正确的结果状态、状态是否仍然活着。

**问：线程池有任务却不执行，应该先增加线程数吗？**

先看现有 worker 在做什么。如果全部在等待需要它们执行的后续任务，增加数量只能推迟触发。

**问：`wait()` 会释放锁，为什么还会死锁？**

它只释放传入的那把 mutex，不会释放业务锁，也不会释放当前任务占用的 worker 执行机会。

**问：用了原子变量，为什么还会出问题？**

原子性不等于整个协议正确。对象的生命周期、多个变量的一致性，以及任务之间的循环依赖，都需要另外保证。

**问：发现卡住后，最先写下什么？**

写下三件事：等待条件、预期完成者、完成者当前的阻塞原因。用这些信息判断是闭环、缺少完成者，还是正常耗时。

## 参考资料

- [本项目 ThreadPool.h](../ThreadPool/ThreadPool.h) 与 [ThreadPool.cpp](../ThreadPool/ThreadPool.cpp)。
- [本项目 Task、Result、Any 联动机制](Task、Result、Any%20联动机制.md)。
- [C++ 标准草案：condition_variable](https://eel.is/c++draft/thread.condition.condvar)：等待时释放锁、唤醒后重新加锁、谓词等待与虚假唤醒。
- [C++ 标准草案：thread 构造](https://eel.is/c++draft/thread.thread.constr)：线程创建失败与线程入口未捕获异常。
- [C++ 标准草案：thread 静态成员](https://eel.is/c++draft/thread.thread.static)：`hardware_concurrency()` 允许返回 0。
- [C++ 标准草案：thread 成员函数](https://eel.is/c++draft/thread.thread.member)：join、detach 与自 join 错误。
- [Visual Studio：用线程视图排查死锁](https://learn.microsoft.com/en-us/visualstudio/debugger/how-to-use-the-threads-window?view=vs-2022)。
- [MSVC AddressSanitizer](https://learn.microsoft.com/cpp/sanitizers/asan)：内存错误检测与构建说明。
- [Clang ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html)：数据竞争检测、使用方式和支持平台。
