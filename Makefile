CFLAGS=-I/usr/include/arm-linux-gnueabihf -I/usr/include/libdrm
LDFLAGS=-L/usr/lib/arm-linux-gnueabihf -lavcodec -lavutil -lavformat -ldrm -lpthread

hello_drmprime: hello_drmprime.o drmprime_out.o

