TARGET=mdmemul
OBJ=\
	atport.o \
	mdmemul.o \
	modem.o \

DEP=$(OBJ:%.o=%.d)

CFLAGS += -Wall -g
DEPFLAGS = -MMD -MP

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

%.o: %.c
	$(CC) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(DEP) $(TARGET)

-include $(DEP)
