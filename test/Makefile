# 定义变量
CC := g++
CFLAGS := -Wall
LFLAGS := -lpthread

# 定义源文件和目标文件
SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)
TARGET := publisher subscriber

.PHONY : all
all : $(TARGET)

publisher : publisher.o
	$(CC) -o $@ $< $(LFLAGS)

subscriber : subscriber.o
	$(CC) -o $@ $< $(LFLAGS)
 
# 构建目标依赖
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
 
# 清理
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET)
