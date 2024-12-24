/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _IPC_TOPIC_H_
#define _IPC_TOPIC_H_

struct topic_mate {
	__user char *topic_name;  // 主题名称
	int name_size;
	int data_type;			  // 主题的数据类型，可自行定义枚举等方式表示不同类型
	int data_size;			  // 主题的数据大小
	__user char *auth_scope;  // 订阅授权范围（"none"、"all" 或具体用户组等）
	int auth_size;
};

struct topic_content {
	union {
		int handle;
		void *ptr;
	} target;
	int type;
	__user char *data;
	int data_size;
};

struct topic_subscribe {
	__user char *topic_name;  // 主题名称
	int name_size;
	__user void *target;
};

#define IPC_TOPIC_CREATE		_IOWR('t', 1, struct topic_mate)
#define IPC_TOPIC_PUBLISH		_IOW('t', 2, struct topic_content)
#define IPC_TOPIC_SUBSCRIBE		_IOW('t', 3, struct topic_subscribe)
#define IPC_TOPIC_DELETE		_IOW('t', 4, __s64)
#define IPC_TOPIC_GET			_IOR('t', 5, struct topic_content)

#endif