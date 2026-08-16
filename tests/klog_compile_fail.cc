#include "kernel/log.hpp"

KLOG_MODULE(test);

void invalid_klog_call() { KLOG_INFO("{:d}", "not an integer"); }
