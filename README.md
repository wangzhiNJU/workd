我开发的时候是在debug目录下，用我以前写的文件测试，测好之后就添加进去rdma目录，再去ceph下编译，运行，用test_perf_msgr_**测试；

在debug目录下，cpyread.cpp regread.cpp 是我测试bulk transfer， xx.cpp 是我测试low-latency的文件， huge_page.c是申请大页表内存

在rdma目录下，大致按照PosixStack和DPDKStack， 以及之前Infiniband的框架，和我在debug中的代码合并的。
