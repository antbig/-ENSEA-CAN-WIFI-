GCC = aarch64-linux-gnu-gcc
BIN = server

LIB_WEBSOCKET_PATH = ../libwebsockets/build

all: 
	$(GCC) -o server main.c -I$(LIB_WEBSOCKET_PATH)/include/ -L$(LIB_WEBSOCKET_PATH)/lib/ -lwebsockets -lpthread

clean:
	rm -f $(BIN)
	rm -f *.o
	rm -f *~

