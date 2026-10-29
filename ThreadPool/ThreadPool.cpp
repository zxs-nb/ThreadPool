#include "ThreadPool.h"
#include <functional>
#include <thread>
#include <iostream>


const int TASK_MAX_THRESHHLOD = 1024;

//线程池的构造
ThreadPool::ThreadPool() : 
	initThreadSize_(0), 
	taskSize_(0), 
	taskQueMaxThreshHold_(TASK_MAX_THRESHHLOD),
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
void ThreadPool::submitTask(std::shared_ptr<Task> sp) {
	//获取锁
	std::unique_lock<std::mutex> unique_lock(taskQueMtx_);

	//线程的通信 等待队列有空余
	//用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
	notFull.wait(unique_lock, [this]() {
		return taskSize_ < TASK_MAX_THRESHHLOD;
	});

	//如果有空余，把任务放入任务队列
	TaskQue_.push(sp);
	taskSize_++;

	//放入了新任务，任务队列肯定不为空，在notEmpty上通知消费线程
	notEmpty.notify_all();
}

//开启线程池
void ThreadPool::start(int initThreadSize) {
	//记录初始线程个数
	initThreadSize_ = initThreadSize;

	//创建线程对象
	for (int i = 0; i < initThreadSize_; ++i) {
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::ThreadFunc, this));
		threads_.emplace_back(std::move(ptr));
	}

	//启动所有线程
	for (int i = 0; i < initThreadSize_; ++i) {
		threads_[i]->start();
	}
}

//线程执行函数       线程池的所有线程从任务队列里消费任务，notEmpty
void ThreadPool::ThreadFunc() {
	std::lock_guard<std::mutex> lock(taskQueMtx_);
	std::cout << "begin threadFunc tid : " << std::this_thread::get_id() << std::endl;
	std::cout << "end threadFunc tid : " << std::this_thread::get_id() << std::endl;
}

//线程构造
Thread::Thread(ThreadFunc func) : func_(func) {

}

//线程析构
Thread::~Thread() { }


//启动线程
void Thread::start() {
	//创建一个线程来执行一个线程函数
	std::thread t(func_);   // C++11来说 线程对象t 和线程函数func_
	t.detach();       // 设置分离线程  pthread_detach   pthread_t设置成分离线程
}