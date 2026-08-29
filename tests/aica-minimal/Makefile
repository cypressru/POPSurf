TARGET = aicatest.elf
OBJS = main.o
all: $(TARGET)
include $(KOS_BASE)/Makefile.rules
clean:
	-rm -f $(TARGET) $(OBJS)
$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS)
