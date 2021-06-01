#include "Demuxer.h"

#include <pthread.h>
#include <unistd.h>

#include <opencv2/highgui/highgui_c.h>

using namespace std;
using namespace cv;

typedef struct {
	BitStream *input_stream;
	queue<Mat *> *video_buffer;
} VideoThreadArgs;

void* decode_video_thread(void*);

int main(int argc, char** argv) {
	printf("MPEG1 player\n");
	printf("Playing %s\n", argv[1]);

	queue<Mat *> *display_buffer = new queue<Mat *>();
	
	time_t t1 = time(NULL);

	Demuxer *demuxer = new Demuxer(argv[1]);

	VideoThreadArgs *video_args = (VideoThreadArgs*)malloc(sizeof(VideoThreadArgs));
	video_args->input_stream = demuxer->video_stream;
	video_args->video_buffer = display_buffer;

	pthread_t video_thread;

	pthread_create(&video_thread, NULL, decode_video_thread, (void*)video_args);

	usleep(40*25*10 * 1000);
	int delay = 1000/30 - 10;
	Mat *display;

	while(true) {
		while(display_buffer->empty()) {
			usleep(100* 1000);
		}

		display = display_buffer->front();
		display_buffer->pop();

		imshow("Display window", *display);
		if(cvWaitKey(delay)==27) {
			break;
		}
	}

	pthread_join(video_thread, NULL);

	return 1;
}

void* decode_video_thread(void *args) {
	auto video_args = (VideoThreadArgs*)args;
	auto video_stream = (BitStream*)video_args->input_stream;
	auto video_buffer = (queue<Mat *>*)video_args->video_buffer;

	VideoDecoder *video_decoder = new VideoDecoder(video_stream, video_buffer);
	video_decoder->decode();

	pthread_exit(NULL);
}
