# pack手順
```
$ cd mruby/pack-test
$ cat ../src/*.c > mruby.c
$ cat ../build/host/mrblib/mrblib.c >> mruby.c 
$ cp ../src/value_array.h .
$ gcc -I ../include/ mruby.c -lm
```

# 重複しているコード
## segment
hash.c
variable.c

## #define CASE
codedump.c
vm.c

## tempirep_free
load.c
proc.c

## ci_nregs
gc.c
vm.c
