#pragma once
#include <vector>
#include <stdint.h>

using Buffer = std::vector<uint8_t>;

struct Conn {
	int fd = -1;

	bool want_read = false;
	bool want_write = false;
	bool want_close = false;

	Buffer incoming; // allocatin buffer for incomplete incoming 
	//streams
	Buffer outgoing; // responses waiting for the os to write them
};