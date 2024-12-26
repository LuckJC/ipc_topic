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

#include "ipc_topic.h"

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
void print_help(const char *program_name) {
    printf("Usage: %s <topic_name>\n", program_name);
    printf("  <topic_name>: The topic to be subscribe (string).\n");
}

// 处理获取到的主题内容的函数，目前只是简单打印数据示例，可根据实际需求扩展功能
void deal_event(struct topic_content *content) {
    printf("content: %s\n", content->data);
}

typedef void *(*work_func_t)(struct topic_content *content);

int main(int argc, char *argv[]) {
    int fd;
    struct topic_subscribe subscribe;
    struct topic_content content;
    struct pollfd fds[1];
    char *topic_name;
    int name_size;
    int ret = 0;

    // 参数个数检查
    if (argc!= 2) {
        print_help(argv[0]);
        exit(EXIT_FAILURE);
    }

    // 打开设备文件（对应内核模块创建的主题相关设备）
    fd = open("/dev/ipc_topic", O_RDWR);
    if (fd == -1) {
        ERROR_EXIT("open");
    }

    // 获取命令行参数并转换为相应类型
    topic_name = argv[1];
    name_size = strlen(topic_name) + 1;

    subscribe.name_size = name_size;
    subscribe.topic_name = topic_name;
    subscribe.target = deal_event;

    // 使用ioctl调用内核模块的订阅主题数据功能（假设IPC_TOPIC_SUBSCRIBE是对应的命令）
    perform_ioctl(fd, IPC_TOPIC_SUBSCRIBE, &subscribe);

    // 设置要监控的文件描述符，这里监控标准输入（文件描述符为0）
    fds[0].fd = fd;
    // 监控可读事件
    fds[0].events = POLLIN;
    // 初始化为0，表示没有事件发生
    fds[0].revents = 0;

    while (1) {
        // 调用poll函数进行监控，超时时间设置为 -1，表示无限等待直到有事件发生
        ret = poll(fds, 1, -1);
        if (ret == -1) {
            perror("poll error");
            return -1;
        } else if (ret == 0) {
            // 超时情况，这里由于设置为无限等待，正常不会进入此分支
            printf("poll timeout\n");
            continue;
        }

        // 判断标准输入是否有可读事件发生
        if (fds[0].revents & POLLIN) {
            // 使用ioctl调用内核模块获取主题数据（假设IPC_TOPIC_GET是对应的命令）
            perform_ioctl(fd, IPC_TOPIC_GET, &content);
            // 调用处理函数处理获取到的主题内容
            ((work_func_t)content.target.ptr)(&content);
        }
    }

    // 关闭设备文件
    if (close(fd) == -1) {
        ERROR_EXIT("close");
    }

    return 0;
}