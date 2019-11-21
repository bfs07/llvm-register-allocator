NO_COLOR=\033[0m
OK_COLOR=\033[32;01m
WARN_COLOR=\033[33;01m

DONE_STRING=$(OK_COLOR)Done.$(NO_COLOR)
COMPILING_STRING=$(WARN_COLOR)Compiling...$(NO_COLOR)

PRINT_DONE=@echo "$(DONE_STRING)"
PRINT_COMPILING=@echo "$(COMPILING_STRING)"


compile:
	$(PRINT_COMPILING)
	g++ -fPIC -shared -I . RAColorBasedCoalescing.cpp CodeGen/*.cpp -o libRegAllocColor.so `llvm-config-4.0 --cxxflags`
	$(PRINT_DONE)
run:
	clang-4.0 -c -emit-llvm tests/main.c -o tests/main.bc
	llc-4.0 -load ./libRegAllocColor.so -regalloc=colorBased tests/main.bc -o tests/main.s
