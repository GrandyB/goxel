/*
	A Simple Yet Weird Way Of Frame Capping.
    from goxel2 project - https://github.com/pegvin/goxel2/commit/d1aa85f103f2510e62ae21b03a6ac35d753d7fcc
*/

#include <chrono>
#include <thread>

#include "framecap.h"

std::chrono::milliseconds wait_time;
std::chrono::time_point<std::chrono::steady_clock> start_time;
std::chrono::time_point next_time = start_time + wait_time;

void framecap_init(void) {
	wait_time = std::chrono::milliseconds{ 17 };
	start_time = std::chrono::steady_clock::now();
}

void framecap_sleep(void) {
	std::this_thread::sleep_until(next_time);
	next_time += wait_time;
}