SOURCE  := $(wildcard *.c)
OBJS    := $(patsubst %.c, %.o, $(SOURCE))
CC      := gcc
CFLAGS  := -Wall -Wextra -Wno-unused-parameter -Wno-pointer-arith -Wno-strict-aliasing -g -std=gnu11 -MMD -MP

.PHONY : all clean

TARGET  := libasnw.a

all : $(TARGET)

clean :
	rm -rf *.d *.o $(TARGET)

$(TARGET) : $(OBJS)
	ar -rs $@ $^
.c.o :
	$(CC) $(CFLAGS) -c -o $@ $< $(INCS)

-include $(OBJS:.o=.d)
