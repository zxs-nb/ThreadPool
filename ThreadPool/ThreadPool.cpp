#include "ThreadPool.h"
#include <functional>
#include <thread>
#include <iostream>
#include <chrono>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHLOD = 10;
const int THREAD_MAX_IDLE_TIME = 60;    //线程最大空闲时间(s)

//线程池的构造
ThreadPool::ThreadPool() :
	initThreadSize_(0),
	curThreadSize_(0),
	taskSize_(0),
	idleThreadSize_(0),
	taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD),
	threadSizeThreshHold_(THREAD_MAX_THRESHHLOD),
	poolMode_(PoolMode::MODE_FIXED),
	isPoolRunning_(false)
{ }

//线程池的析构
ThreadPool::~ThreadPool()
{
	//1.修改线程池状态，告诉所有子线程:线程池要关闭了
	isPoolRunning_ = false;

	//2.获取任务队列锁，准备唤醒所有睡眠等待任务的线程
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	//3.唤醒卡在notEmpty等待任务的工作线程
	notEmpty.notify_all();

	//4.阻塞等待所有子线程正确退出并从threads_中把自己擦除
	exitCond_.wait(lock, [this]() ->bool {
		return threads_.empty();
	});
	std::cout << "ThreadPool::~ThreadPool() 析构成功，所有线程已安全回收！" << std::endl;
}

//判断线程池的运行状态
bool ThreadPool::checkRunningState() const {
	return isPoolRunning_;
}


//设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode) {
	if (checkRunningState()) {
		return;
	}
	poolMode_ = mode;
}


//设置task任务队列的上限阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold) {
	if (checkRunningState()) {
		return;
	}
	taskQueMaxThreshHold_ = threshhold;
}

//设置线程池cached模式下线程阈值
void ThreadPool::setThreadSizeThreshHold(int threshhold) {
	if (checkRunningState()) {
		return;
	}
	if (poolMode_ == PoolMode::MODE_CACHED) {
		threadSizeThreshHold_ = threshhold;
	}
}

//给线程池提交任务      用户通过提供的接口生产任务notFull
Result ThreadPool::submitTask(std::shared_ptr<Task> sp) {
	//获取锁
	std::unique_lock<std::mutex> unique_lock(taskQueMtx_);

	//线程的通信 等待队列有空余
	//用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
	bool hasSpace = notFull.wait_for(unique_lock, std::chrono::seconds(1), [this]() {
		return taskSize_ < taskQueMaxThreshHold_;
	});
	if (!hasSpace) {
		std::cerr << "task queue is Full, submit task failed. " << std::endl;
		return Result(sp, false);
	}

	//如果有空余，把任务放入任务队列
	taskQue_.push(sp);
	taskSize_++;

	//放入了新任务，任务队列肯定不为空，在notEmpty上通知消费线程
	notEmpty.notify_all();

	//需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
	if (poolMode_ == PoolMode::MODE_CACHED
		&& taskSize_ > idleThreadSize_
		&& curThreadSize_ < threadSizeThreshHold_) 
	{
		std::cout << ">>> create new thread " << std::this_thread::get_id() << std::endl;
		//创建新线程
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		threads_[threadId]->start();   //启动新的线程
		curThreadSize_++;
		idleThreadSize_++;
	}


	//返回任务的Result对象
	return Result(sp);
}

//开启线程池
void ThreadPool::start(int initThreadSize) {
	isPoolRunning_ = true;
	//记录初始线程个数
	initThreadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;
	//创建线程对象
	for (int i = 0; i < initThreadSize_; ++i) {
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
	}

	//启动所有线程
	for (int i = 0; i < initThreadSize_; ++i) {
		threads_[i]->start();
		idleThreadSize_++;   //记录空闲线程数量
	}
}

//线程执行函数       线程池的所有线程从任务队列里消费任务，notEmpty
void ThreadPool::threadFunc(int threadId) {
	auto lastTime = std::chrono::high_resolution_clock().now();
	for (;;) {
		std::shared_ptr<Task> task;
		{
			//获取锁
			std::unique_lock<std::mutex> unique_lock(taskQueMtx_);

			std::cout << "tid: " << std::this_thread::get_id()
				<< "尝试获取任务..." << std::endl;

			while (taskSize_ == 0) {
				if (!isPoolRunning_) {
					threads_.erase(threadId);
					curThreadSize_--;
					idleThreadSize_--;
					exitCond_.notify_all();  //唤醒析构函数，检查是否满足析构完全退出条件
					std::cout << "线程 " << std::this_thread::get_id() << "退出！" << std::endl;
					return;
				}
				//cached模式下，可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程结束回收
				if (poolMode_ == PoolMode::MODE_CACHED) {
					//每一秒钟返回一次来确定是否超时
					if (std::cv_status::timeout ==
						notEmpty.wait_for(unique_lock, std::chrono::seconds(1)))
					{
						auto now = std::chrono::high_resolution_clock().now();
						auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
						if (dur.count() >= THREAD_MAX_IDLE_TIME
							&& curThreadSize_ > initThreadSize_) {
							curThreadSize_--;
							idleThreadSize_--;
							threads_.erase(threadId);
							std::cout << "线程 " << std::this_thread::get_id() << "退出！" << std::endl;
							return;
						}
					}
				}
				else {
					notEmpty.wait(unique_lock);    //为固定线程模式，释放锁
				}
			}

			std::cout << "this thread tid: " << std::this_thread::get_id()
				<< "获取任务成功..." << std::endl;

			//任务队列不为空，取出任务执行
			idleThreadSize_--;     //空闲线程减一
			task = std::move(taskQue_.front()); //取出任务队列的任务
			taskQue_.pop();  //从任务队列弹出取出的任务
			taskSize_--;     //任务数量减一
			
			//若任务队列不为空，唤醒等待任务的线程
			if (taskSize_ > 0) {
				notEmpty.notify_all();
			}

			//取出任务，进行通知
			notFull.notify_all();
		}

		//当前线程执行这个任务
		if (task != nullptr) {
			task->exec();
		}

		//已经处理完任务，该线程恢复空闲状态，空闲线程数+1
		idleThreadSize_++;
		lastTime = std::chrono::high_resolution_clock().now();
	}
}


//***************线程方法实现
int Thread::generateId_ = 0;

//线程构造
Thread::Thread(ThreadFunc func) 
	: func_(func)
	, threadId_(generateId_++)
{ }

//线程析构
Thread::~Thread() { }


//启动线程
void Thread::start() {
	//创建一个线程来执行一个线程函数
	std::thread t(func_, threadId_);   // C++11来说 线程对象t 和线程函数func_
	t.detach();       // 设置分离线程  pthread_detach   pthread_t设置成分离线程
}

//获取线程id
int Thread::getId()const {
	return threadId_;
}

//**************Result类实现
Result::Result(std::shared_ptr<Task> task, bool isValid) 
	:task_(task) 
	,isValid_(isValid)
{
	task_->setResult(this);
}

void Result::setValue(Any any) {
	this->any_ = std::move(any);
	sem_.post();
}

Any Result::get() {
	if (!isValid_) {
		return "";
	}
	sem_.wait();    //task任务没有执行完，会阻塞用户的线程
	return std::move(any_);
}



// *************Task方法实现:

Task::Task()
	:result_(nullptr)
{ }


void Task::exec() {
	if (result_ != nullptr) {
		result_->setValue(run());
	}
}

void Task::setResult(Result* res) {
	result_ = res;
}