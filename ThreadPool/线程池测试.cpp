#include <iostream>
#include "ThreadPool.h"
#include <chrono>

int main()
{
	ThreadPool threadPool;
	threadPool.start();

	return 0;
}