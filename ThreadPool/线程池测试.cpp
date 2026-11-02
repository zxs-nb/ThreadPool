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
		threadPool.setThreadSizeThreshHold(8);
		threadPool.start();
		Result res = threadPool.submitTask(std::make_shared<MyClass>());
		Result res1 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res2 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res3 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res4 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res5 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res6 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res7 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res8 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res9 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res10 = threadPool.submitTask(std::make_shared<MyClass>());
		Result res11 = threadPool.submitTask(std::make_shared<MyClass>());
		int sum = res.get().cast_<int>();
		std::cout << "计算结果为: " << sum << std::endl;
		getchar();
	}
	catch (const std::exception& e) {
		std::cout << "Exception is " << e.what() << std::endl;
	}
	return 0;
}