# Task、Result、Any 联动机制

## 1. 三个类分别解决什么问题？

**一句话总结：Task 负责计算，Any 负责保存不同类型的值，Result 负责等待并取回计算结果。**

|类|职责|核心思想|
|---|---|---|
|`Task`|抽象任务并执行用户业务逻辑|继承、多态、模板方法思想|
|`Any`|统一封装不同类型的返回值|类型擦除、模板、多态|
|`Result`|保存异步结果，协调线程间通信|信号量、结果交付、生命周期管理|

线程池的核心问题是：调用者提交任务后，任务并不是立即执行完毕，而是由其他工作线程异步执行。

因此必须同时解决：

1. 如何让不同任务使用统一的执行接口？
    
2. 如何处理不同任务返回值类型不相同的问题？
    
3. 工作线程如何将结果安全交还给调用线程？
    

这三个问题分别由 `Task`、`Any` 和 `Result` 解决。

## 2. 三者之间的调用关系

```
用户线程
   |
   v
submitTask(shared_ptr<Task>)
   |
   +----> 将 Task 加入任务队列
   |
   +----> 创建并返回 Result
              |
              v
       task_->setResult(this)
              |
              v
       Task 保存 Result*
              |
              |
工作线程取出 Task
   |
   v
Task::exec()
   |
   v
Task::run()       <-- 动态绑定到具体派生类
   |
   v
返回 Any
   |
   v
Result::setVal()
   |
   +----> any_ = std::move(any)
   |
   +----> sem_.post()
              |
              v
       用户线程 get() 结束等待
              |
              v
       cast_<T>() 获取实际值
```

## 3. Task：业务逻辑与框架逻辑分离

核心代码：

```
void Task::exec()
{
    if (result_ != nullptr)
    {
        result_->setVal(run());
    }
}
```

这里的 `run()` 是虚函数，实际执行的是用户派生类重写的 `run()`。

**为什么还需要** `**exec()**`**，而不是直接调用** `**run()**`**？**

因为 `run()` 只负责计算，并不知道如何将计算结果交还给调用者。

`exec()` 将执行任务与结果交付组合在一起：

- `run()`：业务逻辑。
    
- `exec()`：框架执行逻辑。
    
- `setVal()`：保存结果并通知调用线程。
    

这体现了模板方法思想：**框架控制流程，用户只实现可变的业务步骤。**

## 4. Any：类型擦除

`Task::run()` 统一返回 `Any`，但 `Any` 内部可以保存 `int`、`double`、`std::string` 等不同类型的值。

关键成员：

```
class Base {
public:
    virtual ~Base() = default;
};

template<typename T>
class Derive : public Base {
public:
    Derive(T data) : data_(data) {}

    T data_;
};

std::unique_ptr<Base> base_;
```

以 `Any a = 100;` 为例：

1. 编译器推导出 `T = int`。
    
2. 创建 `Derive<int>` 对象。
    
3. 使用 `unique_ptr<Base>` 管理对象。
    
4. 调用 `cast_<int>()` 时，通过 `dynamic_cast` 检查真实类型。
    
5. 类型匹配后，返回保存的整数。
    

**设计本质：使用模板保存真实类型，使用基类指针统一存储形式，在取值时恢复类型。**

注意，`Any` 本身不负责线程同步。

## 5. Result：异步任务的结果容器

关键代码：

```
void Result::setVal(Any any)
{
    any_ = std::move(any);
    sem_.post();
}

Any Result::get()
{
    // 此处省略提交失败检查
    sem_.wait();
    return std::move(any_);
}
```

这里的 `Semaphore` 初始计数通常为零。

如果用户先调用 `get()`，当前线程会因为没有信号量资源而阻塞。

如果工作线程先完成任务，`post()` 会将计数增加，之后用户调用 `get()` 可以直接获取结果。

因此，不管两个线程的执行顺序如何，正常情况下都能完成结果交付。

**核心原则：先保存结果，再发出完成通知。**

## 6. Task 与 Result 为什么相互持有指针？

教学版中：

```
// Result
std::shared_ptr<Task> task_;

// Task
Result* result_;
```

`Result` 保存 `shared_ptr<Task>`，可以延长任务对象生命周期。

`Task` 保存 `Result*`，使工作线程完成计算后能够找到对应的结果对象。

两者的关系是：

- `Result` 拥有 `Task` 的共享所有权。
    
- `Task` 仅保存指向 `Result` 的非拥有指针。
    

这种方式避免了两者互相使用 `shared_ptr` 造成的循环引用。

但也产生一个重要风险：**如果 Result 先析构，而 Task 后续仍通过裸指针写入结果，就可能发生悬空指针访问。**

更完善的设计应该让两者共同持有堆上的共享结果状态。

## 7. 与 std::future 的关系

教学版实际上是在手动构建异步结果获取机制。

```
教学版：

Task + Any + Result + Semaphore

更成熟的标准库方案：

std::packaged_task + std::future
```

`std::future<T>` 的返回值类型在编译阶段确定，无须使用 `Any` 再进行动态类型转换。

同时，`future` 通过独立共享状态来保存结果或异常，能够更好地管理生命周期。

## 8. 最终总结

这三个类最重要的不是具体代码，而是背后的设计思想：

1. **面向接口编程**：通过 `Task` 的虚函数统一执行不同业务。
    
2. **类型擦除**：通过 `Any` 隐藏不同返回值的具体类型。
    
3. **职责分离**：业务计算、结果存储和线程同步分别由不同组件负责。
    
4. **线程间结果交付**：通过 `setVal()` 和 `get()` 配合信号量完成通知。
    
5. **生命周期管理**：使用智能指针管理资源，但仍需防范裸指针失效。
    

> 最值得记住的执行链：
> 
> `submitTask()` → `Task::exec()` → `run()` → `Result::setVal()` → `sem_.post()` → `Result::get()` → `Any::cast_<T>()`。