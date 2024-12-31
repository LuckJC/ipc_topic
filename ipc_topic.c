/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "ipc_topic.h"

// 主题结构体定义
struct topic_struct {
	int handle;
	char *topic_name;  // 主题名称
	int data_type;	   // 主题的数据类型，可自行定义枚举等方式表示不同类型
	int data_size;	   // 主题的数据大小
	pid_t owner;	   // 主题创建者的用户ID
	char *auth_scope;  // 订阅授权范围（"none"、"all" 或具体用户组等）
	void *last_data;   // 主题最后一次发布的数据指针
	struct topic_proc *proc;
	struct list_head subscribers;  // 订阅该主题的进程链表头
	struct list_head proc_entry;	   // 加入进程 topics
	struct list_head g_entry;		   // 加入全局 g_topic_head
};

struct topic_ref {
	void *target;
	int delete_flag;
	struct topic_struct *topic;
	struct topic_proc *proc;
	struct list_head proc_entry;
	struct list_head subscribe_entry;
	struct list_head event_queue;
};

struct topic_proc {
	int pid;
	int unique_id;
	int event_count;
	wait_queue_head_t wait;
	struct list_head topics;
	struct list_head refs;
	struct list_head delivered_death;
};

struct topic_event {
	uint32_t type;
	char *data;
	int size;
	struct list_head entry;
};

// 全局主题列表头
static struct list_head g_topic_head;
// 互斥锁保护主题相关操作
static DEFINE_MUTEX(topic_lock);

// 安全地复制用户空间字符串到内核空间
static char *safe_memdup_user(const void __user *src, size_t size)
{
	char *dst = kmalloc(size, GFP_KERNEL);
	if (dst && copy_from_user(dst, src, size)) {
		kfree(dst);
		return NULL;
	}
	return dst;
}

// 查找主题（通过主题名称）
static struct topic_struct *find_topic_byname(char *topic_name)
{
	struct topic_struct *topic;
	list_for_each_entry(topic, &g_topic_head, g_entry) {
		if (!strcmp(topic_name, topic->topic_name)) {
			return topic;
		}
	}
	return NULL;
}

// 查找主题（通过进程和主题ID）
static struct topic_struct *find_topic_byid(struct topic_proc *proc, int handle)
{
	struct topic_struct *topic;
	list_for_each_entry(topic, &proc->topics, proc_entry) {
		if (handle == topic->handle) {
			return topic;
		}
	}
	return NULL;
}

// 查找主题引用（通过进程和主题名称）
static struct topic_ref *find_ref_byname(struct topic_proc *proc, char *topic_name)
{
	struct topic_ref *ref;
	list_for_each_entry(ref, &proc->refs, proc_entry) {
		if (ref->topic && !strcmp(topic_name, ref->topic->topic_name)) {
			return ref;
		}
	}
	return NULL;
}

// 创建主题
static struct topic_struct *create_topic(struct topic_mate *tm, struct topic_proc *proc)
{
	char *topic_name;
	struct topic_struct *new_topic;

	// 先查找是否已存在同名主题，若存在则直接返回（这里假设同名主题不应重复创建，可根据实际需求调整逻辑）
	topic_name = safe_memdup_user(tm->topic_name, tm->name_size);
	if (!topic_name) {
		printk(KERN_ERR "create_topic: Failed to copy topic name from user space\n");
		return NULL;
	}
	new_topic = find_topic_byname(topic_name);
	if (new_topic) {
		kfree(topic_name);	// 释放复制的主题名称内存
		printk(KERN_INFO "create_topic: Topic with the same name already exists\n");
		return NULL;
	}

	new_topic = kmalloc(sizeof(struct topic_struct), GFP_KERNEL);
	if (!new_topic) {
		kfree(topic_name);	// 确保分配失败时释放已申请的内存
		printk(KERN_ERR "create_topic: Failed to allocate memory for topic_struct\n");
		return NULL;
	}

	new_topic->handle = proc->unique_id++;
	new_topic->topic_name = topic_name;
	new_topic->data_type = tm->data_type;
	new_topic->data_size = tm->data_size;
	new_topic->owner = current->pid;  // 当前进程作为主题创建者
	new_topic->auth_scope = safe_memdup_user(tm->auth_scope, tm->auth_size);
	if (!new_topic->auth_scope) {
		kfree(topic_name);
		kfree(new_topic);
		printk(KERN_ERR "create_topic: Failed to copy auth scope from user space\n");
		return NULL;
	}
	INIT_LIST_HEAD(&new_topic->subscribers);
	new_topic->last_data = NULL;
	new_topic->proc = proc;

	// 将主题添加到内核主题列表（假设存在全局主题列表头 topic_list）
	list_add_tail(&new_topic->g_entry, &g_topic_head);
	list_add_tail(&new_topic->proc_entry, &proc->topics);

	return new_topic;
}

// 创建主题引用
static struct topic_ref *create_topic_ref(struct topic_proc *proc, struct topic_struct *topic, void *ptr)
{
	struct topic_ref *ref;

	ref = find_ref_byname(proc, topic->topic_name);
	if (ref) {
		pr_debug("ref already exist, use it.");
		return ref;
	}

	ref = kmalloc(sizeof(struct topic_ref), GFP_KERNEL);
	if (!ref) {
		printk(KERN_ERR "create_topic_ref: Failed to allocate memory for topic_ref\n");
		return NULL;  // 返回内存不足错误码
	}

	ref->delete_flag = 0;
	ref->topic = topic;
	ref->proc = proc;
	ref->target = ptr;
	INIT_LIST_HEAD(&ref->event_queue);
	list_add_tail(&ref->proc_entry, &proc->refs);

	return ref;
}

// 向主题引用的事件队列中放入事件
static int put_event(struct topic_ref *ref, struct topic_content *content)
{
	struct topic_event *event;

	event = kmalloc(sizeof(struct topic_event), GFP_KERNEL);
	if (!event) {
		printk(KERN_ERR "put_event: Failed to allocate memory for topic_event\n");
		return -ENOMEM;	 // 返回内存不足错误码
	}

	event->type = content->type;
	event->data = safe_memdup_user(content->data, content->data_size);
	if (!event->data) {	 // 检查数据复制是否成功
		kfree(event);
		printk(KERN_ERR "put_event: Failed to copy data for topic_event\n");
		return -ENOMEM;
	}
	event->size = content->data_size;
	ref->proc->event_count++;
	list_add_tail(&event->entry, &ref->event_queue);

	return 0;
}

// 从主题引用的事件队列中获取事件
static int get_event(struct topic_ref *ref, struct topic_content *content_u)
{
	struct topic_event *event;
	int ret = 0;
	struct topic_content content;

	if (list_empty(&ref->event_queue))
		return -ENOENT;

	if (copy_from_user(&content, content_u, sizeof(content))) {
		return -EFAULT;
	}

	event = list_first_entry(&ref->event_queue, struct topic_event, entry);
	put_user(event->type, &content_u->type);
	put_user(event->size, &content_u->data_size);
	put_user(ref->target, &content_u->target.ptr);
	if (copy_to_user(content.data, event->data, event->size)) {
		ret = -EFAULT;
	}
	ref->proc->event_count--;
	list_del(&event->entry);
	kfree(event->data);
	kfree(event);

	return ret;
}

// 发布主题数据函数
static int publish_topic_data(struct topic_struct *topic, struct topic_content *content)
{
	struct topic_ref *ref;
	int size = topic->data_size;

	if (!topic) {
		printk(KERN_ERR "publish_topic_data: Topic does not exist\n");
		return -EINVAL;	 // 主题不存在错误码
	}
	if (topic->owner != current->pid) {
		printk(KERN_ERR "publish_topic_data: Not the owner of the topic, permission denied\n");
		return -EPERM;	// 非主题创建者无权发布错误码
	}
	if (content->data_size < topic->data_size) {
		size = content->data_size;
	}

	// 释放旧数据内存（如果有）并更新为新数据
	kfree(topic->last_data);
	topic->last_data = safe_memdup_user(content->data, size);
	if (!topic->last_data) {
		printk(KERN_ERR "publish_topic_data: Failed to update topic data\n");
		return -ENOMEM;
	}

	list_for_each_entry(ref, &topic->subscribers, subscribe_entry) {
		if (ref->topic) {
			int put_event_ret = put_event(ref, content);
			if (put_event_ret != 0) {
				printk(KERN_ERR "publish_topic_data: Failed to put event for subscriber\n");
				return put_event_ret;
			}
			wake_up_interruptible(&ref->proc->wait);
		}
	}

	return 0;  // 发布成功
}

// 删除主题函数
static int delete_topic(struct topic_struct *topic)
{
	struct topic_ref *ref, *n_ref;

	if (!topic) {
		printk(KERN_ERR "delete_topic: Topic does not exist\n");
		return -EINVAL;	 // 主题不存在错误码
	}
	if (topic->owner != current->pid) {
		printk(KERN_ERR "delete_topic: Not the owner of the topic, permission denied\n");
		return -EPERM;	// 非主题创建者无权删除错误码
	}

	// 通知订阅进程主题已删除（可选择合适方式通知）
	list_for_each_entry_safe(ref, n_ref, &topic->subscribers, subscribe_entry) {
		list_del(&ref->subscribe_entry);
		list_del(&ref->proc_entry);
		list_add_tail(&ref->proc_entry, &ref->proc->delivered_death);
		if (ref->topic) {
			wake_up_interruptible(&ref->proc->wait);
		}
	}

	list_del(&topic->proc_entry);
	list_del(&topic->g_entry);
	kfree(topic->topic_name);
	kfree(topic->auth_scope);
	kfree(topic->last_data);
	kfree(topic);

	return 0;  // 删除成功
}

static int topic_open(struct inode *nodp, struct file *filp)
{
	struct topic_proc *proc;

	proc = kzalloc(sizeof(*proc), GFP_KERNEL);
	if (proc == NULL) {
		printk(KERN_ERR "topic_open: Failed to allocate memory for topic_proc\n");
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&proc->topics);
	INIT_LIST_HEAD(&proc->refs);
	INIT_LIST_HEAD(&proc->delivered_death);
	proc->event_count = 0;
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
	struct topic_struct *topic, *n_topic;
	struct topic_ref *ref, *n_ref;
	struct topic_event *event, *n_event;

	mutex_lock(&topic_lock);
	list_for_each_entry_safe(topic, n_topic, &proc->topics, proc_entry) {
		int delete_ret = delete_topic(topic);
		if (delete_ret != 0) {
			printk(KERN_ERR "topic_release: Failed to delete topic\n");
		}
	}

	list_for_each_entry_safe(ref, n_ref, &proc->refs, proc_entry) {
		list_del(&ref->subscribe_entry);
		list_del(&ref->proc_entry);
		list_for_each_entry_safe(event , n_event, &ref->event_queue, entry) {
			list_del(&event->entry);
			kfree(event->data);
			kfree(event);
		}
		kfree(ref);
	}

	kfree(proc);
	mutex_lock(&topic_lock);

	return 0;
}

static unsigned int topic_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct topic_proc *proc = filp->private_data;
	struct topic_ref *ref, *n_ref;
	struct topic_event *event, *n_event;

	poll_wait(filp, &proc->wait, wait);

	mutex_lock(&topic_lock);
	if(!list_empty(&proc->delivered_death)) {
		list_for_each_entry_safe(ref, n_ref, &proc->delivered_death, proc_entry) {
			list_del(&ref->proc_entry);
			list_for_each_entry_safe(event, n_event, &ref->event_queue, entry) {
				list_del(&event->entry);
				kfree(event->data);
				kfree(event);
			}
			kfree(ref);
		}
		mutex_unlock(&topic_lock);
		return 0;
	}
	mutex_unlock(&topic_lock);

	if(proc->event_count > 0) {
		return POLLIN | POLLRDNORM;
	}

	return 0;
}

static long topic_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct topic_proc *proc = filp->private_data;
	unsigned int size = _IOC_SIZE(cmd);
	void __user *ubuf = (void __user *)arg;

	mutex_lock(&topic_lock);
	pr_debug("cmd:%d\n", _IOC_NR(cmd));

	switch (cmd) {
		case IPC_TOPIC_CREATE: {
			struct topic_mate tm;
			struct topic_struct *topic;
			// 检查参数大小是否合法
			if (size != sizeof(struct topic_mate)) {
				ret = -EINVAL;
				printk(KERN_ERR "topic_ioctl: Invalid size for IPC_TOPIC_CREATE command\n");
				break;
			}
			// 从用户空间复制数据
			if (copy_from_user(&tm, ubuf, sizeof(tm))) {
				ret = -EFAULT;
				printk(KERN_ERR "topic_ioctl: Failed to copy data for IPC_TOPIC_CREATE command\n");
				break;
			}
			// 创建主题
			topic = create_topic(&tm, proc);
			if (!topic) {
				ret = -ENOMEM;
				printk(KERN_ERR "topic_ioctl: Failed to create topic for IPC_TOPIC_CREATE command\n");
				break;
			}
			put_user(topic->handle, (int *)ubuf);
			break;
		}
		case IPC_TOPIC_PUBLISH: {
			struct topic_struct *topic;
			struct topic_content content;
			if (size != sizeof(struct topic_content)) {
				ret = -EINVAL;
				printk(KERN_ERR "topic_ioctl: Invalid size for IPC_TOPIC_PUBLISH command\n");
				break;
			}
			if (copy_from_user(&content, ubuf, sizeof(content))) {
				ret = -EFAULT;
				printk(KERN_ERR "topic_ioctl: Failed to copy data for IPC_TOPIC_PUBLISH command\n");
				break;
			}
			topic = find_topic_byid(proc, content.target.handle);
			if (!topic) {
				ret = -EINVAL;
				printk(KERN_ERR "topic_ioctl: Topic not found for IPC_TOPIC_PUBLISH command\n");
				break;
			}
			ret = publish_topic_data(topic, &content);
			if (ret != 0) {
				printk(KERN_ERR "topic_ioctl: Failed to publish topic data for IPC_TOPIC_PUBLISH command\n");
				break;
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
				printk(KERN_ERR "topic_ioctl: Invalid size for IPC_TOPIC_SUBSCRIBE command\n");
				break;
			}
			if (copy_from_user(&subscribe, ubuf, sizeof(subscribe))) {
				ret = -EFAULT;
				printk(KERN_ERR "topic_ioctl: Failed to copy data for IPC_TOPIC_SUBSCRIBE command\n");
				break;
			}
			topic_name = safe_memdup_user(subscribe.topic_name, subscribe.name_size);
			topic = find_topic_byname(topic_name);
			if (!topic) {
				ret = -EINVAL;
				printk(KERN_ERR "topic_ioctl: Topic [%s] not found for IPC_TOPIC_SUBSCRIBE command\n", topic_name);
				kfree(topic_name);
				break;
			}
			ref = create_topic_ref(proc, topic, subscribe.target);
			if (!ref) {
				ret = -ENOMEM;
				printk(KERN_ERR "topic_ioctl: Failed to create topic [%s] ref for IPC_TOPIC_SUBSCRIBE command\n", topic_name);
				kfree(topic_name);
				break;
			}
			list_add_tail(&ref->subscribe_entry, &topic->subscribers);
			kfree(topic_name);
			break;
		}
		case IPC_TOPIC_GET: {
			struct topic_ref *ref;
			struct topic_content *content_u = (struct topic_content *)ubuf;

			if (size != sizeof(struct topic_content)) {
				ret = -EINVAL;
				printk(KERN_ERR "topic_ioctl: Invalid size for IPC_TOPIC_GET command\n");
				break;
			}

			if(proc->event_count == 0) {
				ret = -ENOENT;
				break;
			}

			list_for_each_entry(ref, &proc->refs, proc_entry) {
				ret = get_event(ref, content_u);
				if (ret == 0) 
					break;
			}
			break;
		}
		case IPC_TOPIC_DELETE: {
			int handle;
			struct topic_struct *topic;
			if (size != sizeof(struct topic_content)) {
				ret = -EINVAL;
				printk(KERN_ERR "topic_ioctl: Invalid size for IPC_TOPIC_DELETE command\n");
				break;
			}
			get_user(handle, (int __user *)ubuf);
			list_for_each_entry(topic, &proc->topics, proc_entry) {
				if (handle == topic->handle) {
					int delete_ret = delete_topic(topic);
					if (delete_ret != 0) {
						ret = delete_ret;
						printk(KERN_ERR "topic_ioctl: Failed to delete topic for IPC_TOPIC_DELETE command\n");
						break;
					}
				}
			}
			break;
		}
		default:
			ret = -EINVAL;
			printk(KERN_ERR "topic_ioctl: Invalid command\n");
			break;
	}

	mutex_unlock(&topic_lock);
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

static struct miscdevice topic_miscdev = {.minor = MISC_DYNAMIC_MINOR, .name = "ipc_topic", .fops = &topic_fops};

static ssize_t debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (mutex_trylock(&topic_lock)) {
		sprintf(buf, "unlocked\n");
		mutex_unlock(&topic_lock);
	} else {
		sprintf(buf,"locked\n");
	}
	return strlen(buf);
}

static ssize_t debug_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	if(buf[0] == '0') {
		mutex_unlock(&topic_lock);
	} else if(buf[0] == '1') {
		mutex_trylock(&topic_lock);
	}
	return count;
}

static DEVICE_ATTR(debug, S_IWUSR | S_IRUGO, debug_show, debug_store);

static int __init topic_init(void)
{
	int ret;

	ret = misc_register(&topic_miscdev);
	if (ret < 0) {
		return ret;
	}

	ret = device_create_file(topic_miscdev.this_device, &dev_attr_debug);
	if(ret) {
		misc_deregister(&topic_miscdev);
		pr_err("device_create_file (%s) = %d\n", dev_attr_debug.attr.name, ret);
	}

	INIT_LIST_HEAD(&g_topic_head);

	return 0;
}

static void __exit topic_exit(void)
{
	device_remove_file(topic_miscdev.this_device, &dev_attr_debug);
	misc_deregister(&topic_miscdev);
}

module_init(topic_init);
module_exit(topic_exit);

MODULE_AUTHOR("huangchao");
MODULE_LICENSE("GPL");
