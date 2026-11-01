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
		return 3;
	}
};

int main()
{
	try {
		ThreadPool threadPool;
		Result res = threadPool.submitTask(std::make_shared<MyClass>());

		threadPool.start();
		int sum = res.get().cast_<int>();
		std::cout << "计算结果为: " << sum << std::endl;
	}
	catch (const std::exception& e) {
		std::cout << "Exception is " << e.what() << std::endl;
	}
	getchar();
	return 0;
}