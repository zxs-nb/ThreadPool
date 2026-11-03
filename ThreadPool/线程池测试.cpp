#include <iostream>
#include "ThreadPool.h"
#include <chrono>

class MyClass : public Task
{
public:
	Any run() {
		std::cout << "this thread tid: " << std::this_thread::get_id() << std::endl;
		std::this_thread::sleep_for(std::chrono::seconds(5));
		return 3;
	}
};

int main()
{
	try {
		ThreadPool threadPool;
		//用户自己设置线程池的工作模式
		threadPool.setMode(PoolMode::MODE_CACHED);
		//设置线程初始数量，并开启线程工作
		threadPool.start(1);
		threadPool.submitTask(std::make_shared<MyClass>());
		threadPool.submitTask(std::make_shared<MyClass>());
		threadPool.submitTask(std::make_shared<MyClass>());
		threadPool.submitTask(std::make_shared<MyClass>());
		/*Result res = threadPool.submitTask(std::make_shared<MyClass>());
		int sum = res.get().cast_<int>();
		std::cout << "计算结果为: " << sum << std::endl;*/
	}
	catch (const std::exception& e) {
		std::cout << "Exception is " << e.what() << std::endl;
	}
	std::cout << "好酥服呀~" << std::endl;
	return 0;
}