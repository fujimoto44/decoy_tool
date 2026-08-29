CXX = g++
CC = gcc
CXXFLAGS = -O2 -std=c++17 -I.
CFLAGS = -O2 -I.
LIBS = -lcurl -lpthread

OBJS = main.o gamma_picker.o keccak.o random.o hash.o

decoy_tool: $(OBJS)
	$(CXX) $(OBJS) -o decoy_tool $(LIBS)

main.o: main.cpp gamma_picker.h
	$(CXX) $(CXXFLAGS) -c main.cpp -o main.o

gamma_picker.o: gamma_picker.cpp gamma_picker.h rng.h
	$(CXX) $(CXXFLAGS) -c gamma_picker.cpp -o gamma_picker.o

keccak.o: keccak.c
	$(CC) $(CFLAGS) -c keccak.c -o keccak.o

random.o: random.c
	$(CC) $(CFLAGS) -c random.c -o random.o

hash.o: hash.c
	$(CC) $(CFLAGS) -c hash.c -o hash.o

clean:
	rm -f *.o decoy_tool

.PHONY: clean
