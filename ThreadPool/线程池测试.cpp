#include <iostream>
#include "ThreadPool.h"
#include <chrono>

class MyClass : public Task
{
public:
	Any run() {
		std::cout << "this thread tid: " << std::this_thread::get_id() << std::endl;
		std::this_thread::sleep_for(std::chrono::seconds(5));
		std::cout << "wocao wgp" << std::endl;
	}
};

int main()
{
	ThreadPool threadPool;
	Result res = threadPool.submitTask(std::make_shared<MyClass>());
	int sum = res.get().cast_<int>();
	threadPool.submitTask(std::make_shared<MyClass>());
	threadPool.submitTask(std::make_shared<MyClass>());
	threadPool.submitTask(std::make_shared<MyClass>());
	threadPool.submitTask(std::make_shared<MyClass>());
	threadPool.submitTask(std::make_shared<MyClass>());
	threadPool.submitTask(std::make_shared<MyClass>());
	threadPool.start();
	getchar();
	return 0;
}