TARGET := BleepBox
PLATFORM_LDFLAGS := 
RUBBERBAND := rubberband-1.8.2
AUBIO := aubio
PORTAUDIO := pa_stable_v190600_20161030
SMQ_CFLAGS := -DHAVE_SMQ -I../smq/src
SMQ_LDFLAGS :=  -lzmq -luuid -ljson-c -L../smq/lib -lsmq

ifeq ($(OS),Windows_NT)
    CCFLAGS += -D WIN32
    ifeq ($(PROCESSOR_ARCHITEW6432),AMD64)
        CCFLAGS += -D AMD64
    else
        ifeq ($(PROCESSOR_ARCHITECTURE),AMD64)
            CCFLAGS += -D AMD64
        endif
        ifeq ($(PROCESSOR_ARCHITECTURE),x86)
            CCFLAGS += -D IA32
        endif
    endif
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        CCFLAGS += -D LINUX
        PLATFORM_LDFLAGS += -ldl -lasound
    endif
    ifeq ($(UNAME_S),Darwin)
        CCFLAGS += -D OSX
        PLATFORM_LDFLAGS += -framework Carbon -framework AudioUnit -framework AudioToolbox -framework CoreAudio -framework Accelerate
    endif
    UNAME_P := $(shell uname -p)
    ifeq ($(UNAME_P),x86_64)
        CCFLAGS += -D AMD64
    endif
    ifneq ($(filter %86,$(UNAME_P)),)
        CCFLAGS += -D IA32
    endif
    ifneq ($(filter arm%,$(UNAME_P)),)
        CCFLAGS += -D ARM
    endif
endif

CC=gcc
CXX=g++
CFLAGS := -g $(SMQ_CFLAGS) -Ibuild/portaudio/include -Ibuild/aubio/src -Ibuild/$(RUBBERBAND)
CXXFLAGS := $(CFLAGS) -std=c++11
LDFLAGS := libs/libportaudio.a libs/libaubio.a libs/librubberband.a -lm -lpthread -lsndfile -lfftw3 -lsamplerate $(SMQ_LDFLAGS) $(PLATFORM_LDFLAGS)

DEPS := build/$(RUBBERBAND)/lib/librubberband.a \
        build/$(AUBIO)/build/src/libaubio.a \
        build/portaudio/lib/.libs/libportaudio.a
OBJS := build/$(TARGET).o build/AudioFile.o build/md5sum.o

all: $(TARGET)

downloads/$(RUBBERBAND).tar.bz2:
	mkdir -p downloads
	cd downloads ; curl -O https://breakfastquay.com/files/releases/$(RUBBERBAND).tar.bz2

downloads/$(PORTAUDIO).tgz:
	mkdir -p downloads
	cd downloads ; curl -O http://www.portaudio.com/archives/$(PORTAUDIO).tgz

build/$(RUBBERBAND): downloads/$(RUBBERBAND).tar.bz2
	mkdir -p build
	tar -m -C build/ -xvf downloads/$(RUBBERBAND).tar.bz2

build/$(RUBBERBAND)/lib/librubberband.a: build/$(RUBBERBAND)
	mkdir -p build/$(RUBBERBAND)/lib
	cd build/$(RUBBERBAND) ; ./configure ; make -j 4 static

build/$(AUBIO):
	mkdir -p build
	git clone https://git.aubio.org/aubio/aubio build/aubio

build/$(AUBIO)/build/src/libaubio.a: build/$(AUBIO)
	cd build/$(AUBIO) ; make WAFOPTS="--disable-jack --disable-avcodec"

build/portaudio: downloads/$(PORTAUDIO).tgz
	mkdir -p build
	tar -m -C build/ -xvf downloads/$(PORTAUDIO).tgz

build/portaudio/lib/.libs/libportaudio.a: build/portaudio
	cd build/portaudio ; ./configure --disable-mac-universal --without-jack ; make -j 4

libs/librubberband.a: build/$(RUBBERBAND)/lib/librubberband.a
	mkdir -p libs
	cp -f build/$(RUBBERBAND)/lib/librubberband.a libs

libs/libaubio.a: build/$(AUBIO)/build/src/libaubio.a
	mkdir -p libs
	cp -f build/$(AUBIO)/build/src/libaubio.a libs

libs/libportaudio.a: build/portaudio/lib/.libs/libportaudio.a
	mkdir -p libs
	cp -f build/portaudio/lib/.libs/libportaudio.a libs

deplibs: libs/librubberband.a libs/libaubio.a libs/libportaudio.a

build/%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

build/%.o: %.cpp
	$(CXX) -c -o $@ $< $(CXXFLAGS)

$(TARGET): $(DEPS) deplibs $(OBJS)
	$(CXX) -o $(TARGET) $(OBJS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
	rm -rf libs build downloads
