#pragma once
#include <vector>
#include <memory>
#include <queue>
#include <atomic>
#include <mutex>
#include <functional>
#include <exception>


//Any类型:可以接受任意数据的类型
class Any
{
public:
	Any() = default;
	~Any() = default;
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;

	//让Any能接收任意类型数据
	template<class T>
	Any(T data) : base_(std::make_unique<Derived<T>>(data))
	{ }

	//把Any对象里存储的data数据提取出来
	template<class T>
	T cast_()
	{
		//基类指针转成派生类指针
		Derived<T>* pd = dynamic_cast<Derived<T>*>(base_.get());
		if (pd == nullptr) {
			throw std::runtime_error("type is unmatch");
		}
		return pd->data_;
	}

private:
	//基类类型
	class Base
	{
	public:
		virtual ~Base() = default;
	};

private:
	//派生类类型
	template<class T>
	class Derived : public Base
	{
	public:
		Derived(T data) : data_(data)
		{}
		T data_;        //保存任意的其他类型
	};

private:
	//定义一个基类指针
	std::unique_ptr<Base> base_;
};

class Result;
//任务抽象基类
class Task
{
public:
	Task();
	~Task() = default;
	virtual Any run() = 0;
	void exec();
	void setResult(Result* res);

private:
	Result* result_;
};

//实现一个信号量类
class Semaphore
{
public:
	Semaphore(int limit = 0) : resLimit_(limit) {}
	~Semaphore() = default;

	//获取一个信号量资源
	void wait()
	{
		std::unique_lock<std::mutex> lock(mtx_);
		cond_.wait(lock, [&]()->bool {return resLimit_ > 0; });
		resLimit_--;
	}

	//增加一个信号量资源
	void post()
	{
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;
		cond_.notify_all();
	}

private:
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

//实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
	Result(std::shared_ptr<Task>, bool isValid = true);
	~Result() = default;

	//将Task对象成员方法run得到的返回值放入Result对象内
	void setValue(Any any);

	//阻塞读取任务值，返回给调用者
	Any get();

private:
	Any any_;     //存储任务的返回值
	Semaphore sem_;  //线程通信信号量
	std::shared_ptr<Task> task_; //指向对应获取返回值的任务对象
	bool isValid_;  //返回值是否有效
};

//线程支持的模式
enum class PoolMode
{
	MODE_FIXED,
	MODE_CACHED
};

class Thread
{
public:
	using ThreadFunc = std::function<void()>;

	//线程构造
	Thread(ThreadFunc func);

	//线程析构
	~Thread();

	// 启动线程
	void start();
private:
	ThreadFunc func_;
};


class ThreadPool
{
public:
	ThreadPool();
	~ThreadPool();

	//开启线程池
	void start(int initThreadSize = 4);
	
	//设置线程池的工作模式
	void setMode(PoolMode mode);

	//设置task任务队列的上限阈值
	void setTaskQueMaxThreshHold(int threshhold);

	//给线程池提交任务
	Result submitTask(std::shared_ptr<Task> sp);

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	//线程执行函数
	void threadFunc();

	std::vector<std::unique_ptr<Thread>> threads_;     //线程列表
	int initThreadSize_;       //初始的线程数量

	std::queue<std::shared_ptr<Task>> taskQue_;   //任务队列
	std::atomic_int taskSize_;   //任务的数量
	int taskQueMaxThreshHold_;   //任务队列的数量上限阈值

	std::mutex taskQueMtx_;      //保证任务队列的线程安全
	std::condition_variable notFull; //表示任务队列不满
	std::condition_variable notEmpty; //表示任务队列不空

	PoolMode poolMode_;   //当前线程池的模式
};

