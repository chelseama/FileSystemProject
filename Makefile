CFLAGS = -c -g -Wall -std=gnu99 `pkg-config fuse --cflags --libs`

LDFLAGS = `pkg-config fuse --cflags --libs`

# Uncomment on of the following three lines to compile
SOURCES= disk_emu.c sfs_api.c sfs_test.c sfs_api.h bitmap.c bitmap.h
#SOURCES= disk_emu.c sfs_api.c sfs_test2.c sfs_api.h bitmap.c bitmap.h
#SOURCES= disk_emu.c sfs_api.c fuse_wrappers.c sfs_api.h bitmap.c bitmap.h

#if you wish to create your own test - you can do it using this
#SOURCES= disk_emu.c sfs_api.c sfs_mytest.c sfs_api.h bitmap.c bitmap.h
#SOURCES= disk_emu.c sfs_api.c chelsea_test.c sfs_api.h bitmap.c bitmap.h

OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE= ID_LASTNAME_FIRSTNAME

all: $(SOURCES) $(HEADERS) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	gcc $(OBJECTS) $(LDFLAGS) -o $@

.c.o:
	gcc $(CFLAGS) $< -o $@

clean:
	rm -rf *.o *~ $(EXECUTABLE)
