bashc
=====

A hacked up version of GNU bash that supports a "compiler" mode
wherein instead of executing commands, it generates equivalent C code.
Only a small subset of bash's language features are supported.  There
are no variables or substitutions, but pipes and basic control flow
constructs should work.

While very primitive, it is capable of a funny little bootstrap
maneuver with which you can use this shell-to-C compiler to (in
combination with a C compiler) create a native executable that acts as
a shell-to-native-code compiler:

```
$ cat compiler.sh
./bash --compile /dev/stdout /dev/stdin | gcc -xc - -xnone libbashc.o
$ ./bash compiler.sh < compiler.sh
$ mv a.out shtoelf
$ echo '/bin/echo hello world' | ./shtoelf
$ ./a.out
hello world
```

Note that the resulting executable has no runtime dependency on bash
itself; it performs its own `fork`, `execve`, `pipe` (etc.) calls to
execute the commands in the compiled script directly.

To build a compilation-enabled bash:

```
$ autoconf configure.ac > configure
$ ./configure --enable-compiler
$ make
```

The resulting `bash` binary's `--compile` flag (which can also be
abbreviated `-X`) takes an output file as its argument.

(This hack was originally based on version 4.1 of bash, and only
recently rebased onto a newer upstream.)
