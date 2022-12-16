# lab4: 多核、多进程、调度与IPC

> author：魏新鹏
>
> student ID：519021910888

## 思考题 1

Q：阅读汇编代码 kernel/arch/aarch64/boot/raspi3/init/start.S 。说明ChCore是如何选定主CPU，并阻塞其他其他CPU的执⾏的。

A：From [arm Developer](https://developer.arm.com/documentation/100403/0301/register-descriptions/aarch64-system-registers/mpidr-el1--multiprocessor-affinity-register--el1?lang=en), mpidr_el1系统寄存器的0~7位会存储

> Affinity level 0. The level identifies individual threads within a multithreaded core. 

因此只有0号cpu的x8才是0，因此才会跳转到primary继续执行，其他核心则会在`wait_for_bss_clear`处循环，等待`clear_bss`将`clear_bss_flag`置0后才继续执行。

当0号CPU在`init_c`中调用`clear_bss`后其余核继续执行，将异常等价降低并设置stack pointer后，继续在`wait_until_smp_enabled` loop等待`secondary_boot_flag`变为非0。

## 思考题 2

Q：阅读汇编代码 `kernel/arch/aarch64/boot/raspi3/init/start.S`，`init_c.c` 以及`kernel/arch/aarch64/main.c`，解释⽤于阻塞其他CPU核⼼的`secondary_boot_flag`是物理地址还是虚拟地址？是如何传⼊函数`enable_smp_cores`中，⼜该如何赋值的（考虑虚拟地址/物理地址）？

A：`init_c`中将`secondary_boot_flag`传入`start_kernal`中，`start_kernal`又通过设置`x0`寄存器的值将它传入`main`中，`main`再将它传入`enable_smp_cores`中。

该值是物理地址，因为它在`init_object`段。在`enable_smp_cores`中通过调用`phys_to_virt`将其转换为虚拟地址，从而可以进行赋值。

## 思考题 5

Q：在 el0_syscall 调⽤ lock_kernel 时，在栈上保存了寄存器的值。这是为了避免调⽤ lock_kernel 时修改这些寄存器。在 unlock_kernel 时，是否需要将寄存器的值保存到栈中，试分析其原因。

A：不用将这些寄存器保存到栈中，保存caller-save寄存器主要是为了在之后的syscall对应的函数中使用，调用unlock_kernel时，syscall已经执行完成，故不需要保存。

## 思考题6

Q：为何 idle_threads 不会加⼊到等待队列中？请分析其原因？

A：idel_threads 要调用WFI（wait for interrupt）这是一条特权指令。加入等待队列中的thread在User mode下执行。如果不使用这条指令而是忙等，则会浪费CPU。加入调度队列还会影响其他线程执行效率，每次idle都要执行完时间片或收到中断（其实包含了前者）才会让出CPU。

## 思考题8

Q：如果异常是从内核态捕获的， CPU核⼼不会在 kernel/arch/aarch64/irq/irq_entry.c 的 handle_irq 中获得⼤内核锁。但是，有⼀种特殊情况，即如果空闲线程（以内核态运⾏）中捕获了错误，则CPU核⼼还应该获取⼤内核锁。否则，内核可能会被永远阻塞。请思考⼀下原因。

A：因为空闲线程运行时并没有拿到大内核锁，但是`handle_irq`在返回时会调用`eret_to_thread`释放大内核锁，如果没有`lock_kernel`就调用`unlock`可能会导致排票锁的`owner > next`从而死锁。