CC =		cc
CFLAGS =	-c -fPIC -std=c99 -Wall -Werror -O0 -g -I/usr/local/include
LDFLAGS =	-shared -Wl,-soname,libfmodstudio.so.10 -L/usr/lib -L/usr/local/lib -lopenal -lvorbisfile
RM =		rm -f fmoad-cailin.o al.o fsb.o vorbis_ogg.o vorbis_db.o vorbis_db.c events_db.o events_db.c events_db_parse.o

TARGET_LIB =	libfmodstudio.so.10
SRCS =		fmoad-cailin.c json.c al.c fsb.c vorbis_ogg.c vorbis_db.c events_db.c events_db_parse.c
OBJS =		fmoad-cailin.o json.o al.o fsb.o vorbis_ogg.o vorbis_db.o events_db.o events_db_parse.o

.PHONY: all
all: $(TARGET_LIB)
	ln -sf $(TARGET_LIB) libfmodstudio.so

$(TARGET_LIB): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET_LIB) $(OBJS)

vorbis_db.c: vorbis_db.bin
	xxd -i vorbis_db.bin > vorbis_db.c

vorbis_db.o: vorbis_db.c
	$(CC) $(CFLAGS) vorbis_db.c

events_db.c: events_db.bin
	xxd -i events_db.bin > events_db.c

events_db_parse.o: events_db_parse.c events_db.c
	$(CC) $(CFLAGS) events_db_parse.c

fmoad-cailin.o: fmoad-cailin.c
	$(CC) $(CFLAGS) fmoad-cailin.c

json.o: json.c
	$(CC) $(CFLAGS) json.c

al.o: al.c
	$(CC) $(CFLAGS) al.c

fsb.o: fsb.c
	$(CC) $(CFLAGS) fsb.c

vorbis_ogg.o: vorbis_ogg.c
	$(CC) $(CFLAGS) vorbis_ogg.c

.PHONY: clean
clean:
	$(RM) $(TARGET_LIB) libfmodstudio.so $(OBJS) vorbis_db.c events_db.c
