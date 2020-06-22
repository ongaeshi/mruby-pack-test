# pack手順
```
$ cd mruby/pack-test
$ cat ../src/*.c > mruby.c
$ cat ../build/host/mrblib/mrblib.c >> mruby.c 
$ cp ../src/value_array.h .
$ gcc -I ../include/ mruby.c -lm
```

## 重複しているコード
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

## mrbgemsの結合
- build/host/mrbgems/gem_init.c -> gem_init.c
- gemの基本
  - mrbgems/mruby-array-ext/src/array.c -> pack-test/mruby-array-ext-array.c
  - build/host/mrbgems/mruby-array-ext/gem_init.c -> pack-test/mruby-array-ext-gem_init.c
- mruby-compilerは特殊なので専用処理で書く

## ローカルヘッダファイルの扱いが面倒
- value_array.hとか
- mrbgems以下のtime.hとか
- 1つのアイデアとしてはそのままファイルに流し込んでしまう