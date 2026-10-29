#include <iostream>
#include "ThreadPool.h"
#include <chrono>

int main()
{
	ThreadPool threadPool;
	threadPool.start();
	std::this_thread::sleep_for(std::chrono::seconds(5));
	return 0;
}