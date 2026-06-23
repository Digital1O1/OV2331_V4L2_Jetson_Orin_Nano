CXX      := g++
CXXFLAGS := -g -Wall -std=c++17
LDFLAGS  := $(shell pkg-config --libs opencv4)
INCLUDES := $(shell pkg-config --cflags opencv4)

TARGET   := example-5
SRC      := example-5.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
