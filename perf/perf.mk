# perf/ -- micro-benchmark harness for the per-tick hot functions (pure game/
# code: no GL, no DDS). Built static with frame pointers so `perf record -g`
# gets clean call stacks, and so the binary can be copied to another Linux box.
#   make bench_hot && perf stat ./bench_hot bot 100
#   perf record -g ./bench_hot bot 100 && perf report
bench_hot: perf/bench_hot.cpp $(GAME_SRCS)
	$(CXX) -std=c++17 -O2 -g -fno-omit-frame-pointer -I. -Ithird_party $^ -static -o $@ -lm
