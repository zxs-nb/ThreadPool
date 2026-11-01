#include "ThreadPool.h"
#include <functional>
#include <thread>
#include <iostream>
#include <chrono>

const int TASK_MAX_THRESHHOLD = 1024;
          
//线程池的构造
ThreadPool::ThreadPool() : 
	initThreadSize_(0), 
	taskSize_(0), 
	taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD),
	poolMode_(PoolMode::MODE_FIXED)
{ }

//线程池的析构
ThreadPool::~ThreadPool()
{}


//设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode) {
	poolMode_ = mode;
}


//设置task任务队列的上限阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold) {
	taskQueMaxThreshHold_ = threshhold;
}

//给线程池提交任务      用户通过提供的接口生产任务notFull
Result ThreadPool::submitTask(std::shared_ptr<Task> sp) {
	//获取锁
	std::unique_lock<std::mutex> unique_lock(taskQueMtx_);

	//线程的通信 等待队列有空余
	//用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
	bool hasSpace = notFull.wait_for(unique_lock, std::chrono::seconds(1), [this]() {
		return taskSize_ < TASK_MAX_THRESHHOLD;
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

	//返回任务的Result对象
	return Result(sp);
}

//开启线程池
void ThreadPool::start(int initThreadSize) {
	//记录初始线程个数
	initThreadSize_ = initThreadSize;

	//创建线程对象
	for (int i = 0; i < initThreadSize_; ++i) {
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this));
		threads_.emplace_back(std::move(ptr));
	}

	//启动所有线程
	for (int i = 0; i < initThreadSize_; ++i) {
		threads_[i]->start();
	}
}

//线程执行函数       线程池的所有线程从任务队列里消费任务，notEmpty
void ThreadPool::threadFunc() {
	for (;;) {
		std::shared_ptr<Task> task;
		{
			//获取锁
			std::unique_lock<std::mutex> unique_lock(taskQueMtx_);

			std::cout << "tid: " << std::this_thread::get_id()
				<< "尝试获取任务..." << std::endl;

			//等待notEmpty条件
			notEmpty.wait(unique_lock, [this]() {return taskSize_ > 0; });

			std::cout << "this thread tid: " << std::this_thread::get_id()
				<< "获取任务成功..." << std::endl;

			//从任务队列中取出一个任务
			task = std::move(taskQue_.front());
			taskQue_.pop();
			taskSize_--;

			//如果依然有任务，继续通知其他线程执行任务
			if (!taskQue_.empty()) {
				notEmpty.notify_all();
			}

			//取出任务，进行通知
			notFull.notify_all();
		}

		//当前线程执行这个任务
		if (task != nullptr) {
			task->exec();
		}
	}
}

//线程构造
Thread::Thread(ThreadFunc func) : func_(func) 
{ }

//线程析构
Thread::~Thread() { }


//启动线程
void Thread::start() {
	//创建一个线程来执行一个线程函数
	std::thread t(func_);   // C++11来说 线程对象t 和线程函数func_
	t.detach();       // 设置分离线程  pthread_detach   pthread_t设置成分离线程
}

Result::Result(std::shared_ptr<Task> task, bool isValid) 
	:task_(task) 
	,isValid_(isValid)
{
	task_->setResult(this);
}

void Result::setValue(Any any) {
	sem_.post();
	this->any_ = std::move(any);
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