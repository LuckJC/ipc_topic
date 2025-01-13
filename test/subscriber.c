#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>

#include "ipc_topic.h"

// 定义结构体用于传递给线程的参数
typedef struct {
	int device_fd;	   // 设备文件描述符
	int pipe_read_fd;  // 管道读端文件描述符
} ThreadArgs;

// 宏定义，用于简单的错误处理和打印错误信息后退出程序
#define ERROR_EXIT(msg)     \
	do {                    \
		perror(msg);        \
		exit(EXIT_FAILURE); \
	} while (0)

// 封装ioctl调用及错误处理逻辑，使代码更简洁
void perform_ioctl(int fd, int request, void *arg)
{
	if (ioctl(fd, request, arg) == -1) {
		switch (errno) {
			case EINVAL:
				fprintf(stderr, "Invalid argument passed to ioctl for request code %d.\n", request);
				break;
			case EBADF:
				fprintf(stderr, "Invalid file descriptor for ioctl operation with request code %d.\n", request);
				break;
			case EFAULT:
				fprintf(stderr, "Error accessing user space memory in ioctl operation with request code %d.\n",
						request);
				break;
			default:
				fprintf(stderr, "Unknown error occurred during ioctl for request code %d.\n", request);
		}
		ERROR_EXIT("ioctl operation");
	}
}

// 打印使用帮助信息
void print_help()
{
	printf("Usage:\n");
	printf("  Enter 's <topic_name>' to subscribe a topic.\n");
	printf("  Enter 'quit' to exit the program.\n");
}

// 处理获取到的主题内容的函数，目前只是简单打印数据示例，可根据实际需求扩展功能
void event_handler(struct topic_content *content)
{
	printf("content: %s\n", content->data);
}

void death_handler(struct topic_content *content)
{
	printf("legacy event: %s\n", content->data);
}

typedef void *(*work_func_t)(struct topic_content *content);

// 线程函数，用于在单独线程中获取主题数据
void *get_topic_data_thread(void *arg)
{
	ThreadArgs *args = (ThreadArgs *)arg;
	int fd = args->device_fd;
	int pipe_fd = args->pipe_read_fd;
	struct topic_content content;
	struct pollfd fds[2];  // 用于监控设备文件和管道文件描述符
	char buffer[1024];
	int ret = 0;

	content.data = buffer;

	// 配置要监控的文件描述符及事件类型
	fds[0].fd = fd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	fds[1].fd = pipe_fd;
	fds[1].events = POLLIN | POLLHUP;
	fds[1].revents = 0;

	while (1) {
		// 调用poll函数进行监控，超时时间设置为 -1，表示无限等待直到有事件发生
		ret = poll(fds, 2, -1);
		if (ret == -1) {
			perror("poll error in thread");
			pthread_exit(NULL);
		} else if (ret == 0) {
			// 超时情况，这里由于设置为无限等待，正常不会进入此分支
			continue;
		}

		// 判断设备文件是否有可读事件发生
		if (fds[0].revents & POLLIN) {
			// 使用ioctl调用内核模块获取主题数据（假设IPC_Topic_GET是对应的命令）
			perform_ioctl(fd, IPC_TOPIC_GET, &content);
			// 调用处理函数处理获取到的主题内容
			((work_func_t)content.target.ptr)(&content);
		} else if(fds[0].revents & POLLHUP) {
			printf("bye bye\n");
			perform_ioctl(fd, IPC_TOPIC_GET_LEGACY, &content);
			if(content.target.ptr) {
				((work_func_t)content.target.ptr)(&content);
			}
		}

		// 判断管道是否有可读事件发生（即收到退出通知）
		if (fds[1].revents & POLLIN) {
			char exit_signal;
			if (read(pipe_fd, &exit_signal, 1) > 0) {
				break;
			}
		}
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	int fd;
	struct topic_subscribe subscribe;
	char input[100];
	char topic_name[100];
	int name_size;
	pthread_t thread_id;
	int pipe_fds[2];  // 管道文件描述符数组，pipe_fds[0]为读端，pipe_fds[1]为写端
	ThreadArgs thread_args;

	// 创建管道
	if (pipe(pipe_fds) == -1) {
		ERROR_EXIT("pipe creation error");
	}

	// 打开设备文件（对应内核模块创建的主题相关设备）
	fd = open("/dev/ipc_topic", O_RDWR);
	if (fd == -1) {
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		ERROR_EXIT("open");
	}

	print_help();

	// 初始化传递给线程的参数结构体
	thread_args.device_fd = fd;
	thread_args.pipe_read_fd = pipe_fds[0];

	// 创建并启动获取主题数据的线程，传递参数结构体指针给线程
	if (pthread_create(&thread_id, NULL, get_topic_data_thread, &thread_args) != 0) {
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		close(fd);
		ERROR_EXIT("pthread_create");
	}

	while (1) {
		printf("Please enter command: ");
		fgets(input, sizeof(input), stdin);
		input[strcspn(input, "\n")] = '\0';	 // 去除换行符

		if (strcmp(input, "quit") == 0) {
			// 向管道写入数据，通知线程退出
			char exit_signal = 1;
			if (write(pipe_fds[1], &exit_signal, 1) == -1) {
				perror("write to pipe error");
			}
			pthread_join(thread_id, NULL);
			break;
		} else if (strncmp(input, "s ", 2) == 0) {
			strcpy(topic_name, input + 2);
			topic_name[strcspn(topic_name, "\n")] = '\0';
			name_size = strlen(topic_name) + 1;

			subscribe.name_size = name_size;
			subscribe.topic_name = topic_name;
			subscribe.target = (void *)event_handler;
			subscribe.death_notifier = (void*)death_handler;

			// 使用ioctl调用内核模块的订阅主题数据功能（假设IPC_Topic_SUBSCRIBE是对应的命令）
			perform_ioctl(fd, IPC_TOPIC_SUBSCRIBE, &subscribe);
		} else {
			printf("Invalid command. Please try again.\n");
		}
	}

	// 关闭设备文件和管道文件描述符
	close(fd);
	close(pipe_fds[0]);
	close(pipe_fds[1]);

	return 0;
}