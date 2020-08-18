# mosquitto
LDLIBS+=-lmosquitto

# libevdev
CFLAGS+=`pkg-config --cflags libevdev`
LDLIBS+=`pkg-config --libs libevdev`

# json-c
CFLAGS+=`pkg-config --cflags json-c`
LDLIBS+=`pkg-config --libs json-c`

# General
CFLAGS+=-Wall

all: mqtt2js
js2mqtt: mqtt2js.c

install: mqtt2js
	mkdir -p /usr/local/bin
	cp mqtt2js /usr/local/bin
