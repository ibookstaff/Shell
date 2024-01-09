C = gcc
CFLAGS = -Wall -Werror -pedantic -std=gnu18
LOGIN = bookstaff
SUBMITPATH = ~cs537-1/handin/bookstaff/P3
TARGET = wsh
TAR_FILE = $(LOGIN).tar.gz
all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(TARGET)

tar: $(TARGET).c
	tar -cvf $(TAR_FILE) $^

submit: $(TARGET) tar
	cp $(TAR_FILE) $(SUBMITPATH)

.PHONY: all clean submit