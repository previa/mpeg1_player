#include "Demuxer.h"

int main(int argc, char** argv) {
	printf("MPEG1 player\n");
	printf("Playing %s\n", argv[1]);

	Demuxer demuxer(argv[1]);

	return 1;
}
