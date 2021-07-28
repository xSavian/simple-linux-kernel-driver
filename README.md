Simple character kernel driver for

https://habr.com/ru/post/337946/ - первоисточник 

For develpment

```vagrant up```

Up VM

Build kernel driver:

```
> make all

> sudo insmod main.ko

> dmesg | grep "scull: register device major"
...
"scull: register device major = 243 minor = 0"
...
> sudo mknod /dev/scull c 243 0

> sudo mknod /dev/scull c 243 0

> sudo chmod 777 /dev/scull
```
