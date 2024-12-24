/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Unisoc Inc.
 */

#include <linux/mm.h>
#include <asm/string.h>
#include <asm/uaccess.h>
#include <linux/wait.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/poll.h>

#include "ipc_topic.h"

// 主题结构体定义
struct topic_struct {
	int handle;
	char *topic_name;  // 主题名称
	int data_type;	   // 主题的数据类型，可自行定义枚举等方式表示不同类型
	int data_size;	   // 主题的数据大小
	uid_t owner;	   // 主题创建者的用户ID
	char *auth_scope;  // 订阅授权范围（"none"、"all" 或具体用户组等）
	void *last_data;   // 主题最后一次发布的数据指针
	struct topic_proc *proc;
	struct list_head subscribers;  // 订阅该主题的进程链表头
	struct list_head proc_list;	   // 用于在内核主题列表中链接该主题的节点
	struct list_head list;		   // 用于在内核主题列表中链接该主题的节点
	struct list_head list_p;	   // 用于在内核主题列表中链接该主题的节点
};

struct topic_ref {
	void *target;
	struct topic_struct *topic;
	struct topic_proc *proc;
	struct list_head list;
	struct list_head event_queue;
};

struct topic_proc {
	// struct list_head threads;
	struct list_head topics;
	struct list_head topic_refs;
	int pid;
	int unique_id;
	struct list_head todo;
	wait_queue_head_t wait;
};

struct topic_event {
	uint32_t type;
	char *data;
	int size;
	struct list_head list;
};

enum { IPC_TOPIC_PUBLISHER, IPC_TOPIC_SUBSCRIBER };

static struct list_head topic_list;
static DEFINE_MUTEX(topic_lock);

struct topic_struct *find_topic_byname(char *topic_name)
{
	struct topic_struct *topic;
	list_for_each_entry(topic, &topic_list, list)
	{
		if (!strcmp(topic_name, topic->topic_name)) {
			return topic;
		}
	}
	return NULL;
}

struct topic_struct *find_topic_byid(struct topic_proc *proc, int handle)
{
	struct topic_struct *topic;
	list_for_each_entry(topic, &proc->topics, list_p)
	{
		if (handle == topic->handle) {
			return topic;
		}
	}
	return NULL;
}

static struct topic_ref *find_ref_byname(struct topic_proc *proc, char *topic_name)
{
	struct topic_ref *ref;
	list_for_each_entry(ref, &proc->topic_refs, list)
	{
		if (ref->topic && !strcmp(topic_name, ref->topic->topic_name)) {
			return ref;
		}
	}
	return NULL;
}

// 创建主题函数
static int create_topic(struct topic_mate *tm, struct topic_proc *proc)
{
	char *topic_name;
	// 分配主题结构体内存空间
	struct topic_struct *new_topic;
	topic_name = (char *)memdup_user(tm->topic_name, tm->name_size);
	new_topic = find_topic_byname(topic_name);
	if (new_topic) {
		kfree(new_topic);
		return -EINVAL;
	}

	new_topic = kmalloc(sizeof(struct topic_struct), GFP_KERNEL);
	if (!new_topic) {
		return -ENOMEM;	 // 返回内存不足错误码
	}
	new_topic->handle = proc->unique_id++;
	// 初始化主题名称等属性
	new_topic->topic_name = (char *)memdup_user(tm->topic_name, tm->name_size);
	new_topic->data_type = tm->data_type;
	new_topic->data_size = tm->data_size;
	new_topic->owner = current->pid;  // 当前进程作为主题创建者
	new_topic->auth_scope = memdup_user(tm->auth_scope, tm->auth_size);
	// 初始化订阅者链表等
	INIT_LIST_HEAD(&new_topic->subscribers);
	new_topic->last_data = NULL;
	new_topic->proc = proc;
	// 将主题添加到内核主题列表（假设存在全局主题列表头 topic_list）
	list_add_tail(&new_topic->list, &topic_list);
	list_add_tail(&new_topic->list_p, &proc->topics);
	return (int)new_topic;	// 返回主题结构体指针（可转换为整数便于上层使用）
}

static int create_topic_ref(struct topic_proc *proc, struct topic_struct *topic, void *ptr)
{
	// 分配主题结构体内存空间
	struct topic_ref *ref;
	ref = find_ref_byname(proc, topic->topic_name);
	if (ref)
		return (int)ref;

	ref = kmalloc(sizeof(struct topic_ref), GFP_KERNEL);
	if (!ref) {
		return -ENOMEM;	 // 返回内存不足错误码
	}
	// 初始化主题名称等属性
	ref->topic = topic;
	ref->proc = proc;
	ref->target = ptr;
	list_add_tail(&ref->list, &proc->topic_refs);

	return (int)ref;  // 返回主题结构体指针（可转换为整数便于上层使用）
}

static int put_event(struct topic_ref *ref, struct topic_content *content)
{
	struct topic_event *event;
	event = kmalloc(sizeof(struct topic_event), GFP_KERNEL);
	if (!event) {
		return -ENOMEM;	 // 返回内存不足错误码
	}
	event->type = content->type;
	event->data = memdup_user(event->data, content->data_size);
	event->size = content->data_size;

	list_add_tail(&event->list, &ref->event_queue);

	return (int)event;
}

static int get_event(struct topic_ref *ref, struct topic_content *content_u)
{
	struct topic_event *event;
	int ret = 0;
	struct topic_content content;
	if (list_empty(&ref->event_queue))
		return -EFAULT;

	if (copy_from_user(&content, content_u, sizeof(content))) {
		return -EFAULT;
	}

	event = list_first_entry(&ref->event_queue, struct topic_event, list);
	put_user(event->type, &content_u->type);
	put_user(event->size, &content_u->data_size);
	put_user(ref->target, &content_u->target.ptr);
	if (copy_to_user(content.data, event->data, event->size)) {
		// return ERR_PTR(-EFAULT);
		ret = -EFAULT;
	}
	list_del(&event->list);
	kfree(event->data);
	kfree(event);

	return ret;
}

// 发布主题数据函数
int publish_topic_data(struct topic_struct *topic, struct topic_content *content)
{
	struct topic_ref *ref;
	int size = topic->data_size;
	if (!topic) {
		return -EINVAL;	 // 主题不存在错误码
	}
	if (topic->owner != current->pid) {
		return -EPERM;	// 非主题创建者无权发布错误码
	}
	if (content->data_size < topic->data_size) {
		size = content->data_size;
	}
	// 释放旧数据内存（如果有）并更新为新数据
	kfree(topic->last_data);
	topic->last_data = memdup_user(content->data, size);

	list_for_each_entry(ref, &topic->subscribers, list)
	{
		if (ref->topic) {
			put_event(ref, content);
			wake_up_interruptible(&ref->proc->wait);
		}
	}
	return 0;  // 发布成功
}

// 删除主题函数
int delete_topic(struct topic_struct *topic)
{
	struct topic_ref *ref;
	if (!topic) {
		return -EINVAL;	 // 主题不存在错误码
	}
	if (topic->owner != current->pid) {
		return -EPERM;	// 非主题创建者无权删除错误码
	}
	// 释放主题相关资源（如名称字符串等内存）
	kfree(topic->topic_name);
	kfree(topic->auth_scope);
	// 通知订阅进程主题已删除（可选择合适方式通知）
	list_for_each_entry(ref, &topic->subscribers, list)
	{
		if (ref->topic) {
			wake_up_interruptible(&ref->proc->wait);
		}
	}
	// 从内核主题列表移除主题
	list_del(&topic->list);
	list_del(&topic->list);
	kfree(topic);
	return 0;  // 删除成功
}

static int topic_open(struct inode *nodp, struct file *filp)
{
	struct topic_proc *proc;

	proc = kzalloc(sizeof(*proc), GFP_KERNEL);
	if (proc == NULL)
		return -ENOMEM;
	INIT_LIST_HEAD(&proc->topics);
	INIT_LIST_HEAD(&proc->topic_refs);
	INIT_LIST_HEAD(&proc->todo);
	init_waitqueue_head(&proc->wait);
	mutex_lock(&topic_lock);
	proc->pid = current->group_leader->pid;
	filp->private_data = proc;
	mutex_unlock(&topic_lock);

	return 0;
}

static int topic_release(struct inode *nodp, struct file *filp)
{
	struct topic_proc *proc = filp->private_data;
	// binder_defer_work(proc, BINDER_DEFERRED_RELEASE);

	return 0;
}

static unsigned int topic_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct topic_proc *proc = filp->private_data;

	poll_wait(filp, &proc->wait, wait);
	return 0;
}

static long topic_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;
	struct topic_proc *proc = filp->private_data;
	unsigned int size = _IOC_SIZE(cmd);
	void __user *ubuf = (void __user *)arg;

	mutex_lock(&topic_lock);

	switch (cmd) {
		case IPC_TOPIC_CREATE: {
			struct topic_mate tm;
			struct topic_struct *topic;
			struct topic_ref *ref;
			if (size != sizeof(struct topic_mate)) {
				ret = -EINVAL;
				goto err;
			}
			if (copy_from_user(&tm, ubuf, sizeof(tm))) {
				ret = -EFAULT;
				goto err;
			}
			topic = (struct topic_struct *)create_topic(&tm, proc);
			put_user(topic->handle, (int *)ubuf);
			break;
		}
		case IPC_TOPIC_PUBLISH: {
			int handle;
			struct topic_struct *topic;
			struct topic_content content;
			if (size != sizeof(struct topic_content)) {
				ret = -EINVAL;
				goto err;
			}
			if (copy_from_user(&content, ubuf, sizeof(content))) {
				ret = -EFAULT;
				goto err;
			}
			list_for_each_entry(topic, &proc->topics, list_p)
			{
				if (content.target.handle == topic->handle) {
					publish_topic_data(topic, &content);
				}
			}
			break;
		}
		case IPC_TOPIC_SUBSCRIBE: {
			struct topic_subscribe subscribe;
			struct topic_ref *ref;
			struct topic_struct *topic;
			char *topic_name;
			if (size != sizeof(struct topic_subscribe)) {
				ret = -EINVAL;
				goto err;
			}
			if (copy_from_user(&subscribe, ubuf, sizeof(subscribe))) {
				ret = -EFAULT;
				goto err;
			}
			topic_name = memdup_user(subscribe.topic_name, subscribe.name_size);
			topic = find_topic_byname(topic_name);
			kfree(topic_name);
			if (!topic) {
				ret = -EINVAL;
				goto err;
			}
			ref = create_topic_ref(proc, topic, subscribe.target);
			list_add_tail(&ref->list, &topic->subscribers);
			break;
		}
		case IPC_TOPIC_DELETE: {
			int handle;
			struct topic_ref *ref;
			struct topic_struct *topic;
			if (size != sizeof(struct topic_content)) {
				ret = -EINVAL;
				goto err;
			}
			get_user(handle, (int __user *)ubuf);
			list_for_each_entry(topic, &proc->topics, list_p)
			{
				if (handle == topic->handle) {
					delete_topic(topic);
				}
			}
			break;
		}
		case IPC_TOPIC_GET: {
			int handle;
			struct topic_ref *ref;
			struct topic_struct *ts;
			struct topic_content *content_u = (struct topic_content *)ubuf;

			if (size != sizeof(struct topic_content)) {
				ret = -EINVAL;
				goto err;
			}

			list_for_each_entry(ref, &proc->topic_refs, list)
			{
				if (get_event(ref, content_u) == 0)
					break;
			}
			break;
		}
		//case IPC_TOPIC_VERSION:
		//	break;
		default:
			ret = -EINVAL;
			goto err;
	}
	ret = 0;
err:
	mutex_unlock(&topic_lock);
	//wait_event_interruptible(binder_user_error_wait, binder_stop_on_user_error < 2);
	return ret;
}

const struct file_operations topic_fops = {
	.owner = THIS_MODULE,
	.poll = topic_poll,
	.unlocked_ioctl = topic_ioctl,
	.compat_ioctl = topic_ioctl,
	//.mmap = topic_mmap,
	.open = topic_open,
	//.flush = topic_flush,
	.release = topic_release,
};

static struct miscdevice topic_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ipc_topic",
	.fops = &topic_fops
};

static int __init topic_init(void)
{
	int ret;

	ret = misc_register(&topic_miscdev);
	if (ret < 0) {
		return ret;
	}
	return 0;
}

static void __exit topic_exit(void)
{
	misc_deregister(&topic_miscdev);
}

module_init(topic_init);
module_exit(topic_exit);

MODULE_AUTHOR("huangchao");
MODULE_LICENSE("GPL");
