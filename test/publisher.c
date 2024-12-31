#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

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
void print_help()
{
	printf("Usage:\n");
	printf("  First, you will be prompted to enter topic configuration information.\n");
	printf("  Then, you can enter the topic data to be published.\n");
	printf("  Enter 'quit' to stop publishing topics.\n");
}

int main(int argc, char *argv[])
{
	int fd;
	struct topic_mate mate;
	struct topic_content content;
	char topic_name[100];
	char auth_scope[100];
	char topic_data[1024];
	int data_type, data_size, name_size, auth_size;

	// 打印帮助信息
	print_help();

	// 打开设备文件（对应内核模块创建的主题相关设备）
	fd = open("/dev/ipc_topic", O_RDWR);
	if (fd == -1) {
		ERROR_EXIT("open");
	}

	// 获取主题配置信息（从用户输入）
	printf("Please enter the topic name: ");
	fgets(topic_name, sizeof(topic_name), stdin);
	// 去除换行符，因为fgets会读取换行符
	topic_name[strcspn(topic_name, "\n")] = '\0';
	name_size = strlen(topic_name) + 1;

	printf("Please enter the data type (integer): ");
	scanf("%d", &data_type);
	// 清除输入缓冲区多余的字符（处理scanf遗留的问题）
	while (getchar() != '\n');

	printf("Please enter the data size (integer): ");
	scanf("%d", &data_size);
	while (getchar() != '\n');

	printf("Please enter the auth scope: ");
	fgets(auth_scope, sizeof(auth_scope), stdin);
	auth_scope[strcspn(auth_scope, "\n")] = '\0';
	auth_size = strlen(auth_scope) + 1;

	// 配置topic_mate结构体
	mate.data_type = data_type;
	mate.topic_name = topic_name;
	mate.name_size = name_size;
	mate.data_size = data_size;
	mate.auth_scope = auth_scope;
	mate.auth_size = auth_size;

	// 使用ioctl调用内核模块的发布主题数据功能（假设IPC_TOPIC_CREATE是对应的命令）
	perform_ioctl(fd, IPC_TOPIC_CREATE, &mate);
	content.target.handle = mate.handle;

	while (1) {
		// 获取要发布的主题数据（从用户输入）
		printf("Please enter the topic data: ");
		fgets(topic_data, sizeof(topic_data), stdin);
		topic_data[strcspn(topic_data, "\n")] = '\0';
		// 判断是否输入quit，如果是则退出循环
		if (strcmp(topic_data, "quit") == 0) {
			break;
		}
		data_size = strlen(topic_data) + 1;

		// 填充要发布的主题内容结构体
		content.data = topic_data;
		content.data_size = data_size;

		// 使用ioctl调用内核模块的发布主题数据功能（假设IPC_TOPIC_PUBLISH是对应的命令）
		perform_ioctl(fd, IPC_TOPIC_PUBLISH, &content);

		printf("Topic data published successfully!\n");
	}

	// 关闭设备文件
	if (close(fd) == -1) {
		ERROR_EXIT("close");
	}

	return 0;
}