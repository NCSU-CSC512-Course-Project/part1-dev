# Makefile for KeyPointsCollector
CXX = clang++
CXXFLAGS = -O0 -g3 -std=c++17
LINKER_FLAGS = -lclang

DBG = gdb

BIN_DIR = bin
SRC_DIR = kpc
OBJS_DIR = $(BIN_DIR)/objs

SRC = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJS_DIR)/%.o, $(SRC))
EXE = $(BIN_DIR)/kpc

.PHONY: all main run

all: dirs main

run: all
	$(EXE) test_file.c

dbg: all
	$(DBG) -q --args $(EXE) test_file.c	

main: $(OBJS)
	$(CXX) $(OBJS) $(CXXFLAGS) $(LINKER_FLAGS) -o $(EXE) 

dirs:
	mkdir -p $(BIN_DIR) $(OBJS_DIR)

$(OBJS_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $< 
