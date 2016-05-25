# LLVM Coroutines
## Introduction
bla bla
## Terms
* Coroutine Frame: storage that persist across coroutine suspends and resumes. 
* Coroutine Promise: library defined type that is part of the coroutine frame
* Coroutine Handle: a pointer that encodes information about an  
* Suspend Point
* Final Suspend point

## Coroutine structure intrinsics

Coroutine structure intrinsics 

def int_experimental_coro_init : Intrinsic<[llvm_ptr_ty], [llvm_ptr_ty, llvm_ptr_ty, llvm_ptr_ty], []>;
def int_experimental_coro_fork : Intrinsic<[llvm_i1_ty], [], []>;

<!-- def int_experimental_coro_frame : Intrinsic<[llvm_ptr_ty], [], []>;-->
def int_experimental_coro_size : Intrinsic<[llvm_i32_ty], [], [IntrNoMem]>;

def int_experimental_coro_end : Intrinsic<[], [], []>;

def int_experimental_coro_save : Intrinsic<[llvm_token_ty], [llvm_i32_ty], []>;
def int_experimental_coro_suspend : Intrinsic<[llvm_i1_ty], [llvm_token_ty], []>;

## Library support intrinsics
The following intrinsics are used by the libraries to create high-level coroutine abstractions, such as generator, tasks, etc.

### llvm.experimental.coro.resume
#### Syntax
    declare void
      @llvm.experimental.coro.resume(i8* %handle)
#### Overview
The `@llvm.experimental.coro.resume` intrinsics resumes a coroutine.
#### Arguments
The argument is a coroutine handle returned previously by `@llvm.experimental.coro.from.promise`.
#### Semantics
bla bla

def int_experimental_coro_resume : Intrinsic<[], [llvm_ptr_ty], [Throws]>;
def int_experimental_coro_destroy : Intrinsic<[], [llvm_ptr_ty], [Throws]>;
def int_experimental_coro_done : Intrinsic<[llvm_i1_ty], [llvm_ptr_ty], [IntrReadMem]>;
def int_experimental_coro_promise : Intrinsic<[llvm_anyptr_ty], [llvm_ptr_ty], []>;
def int_experimental_coro_from_promise : Intrinsic<[llvm_ptr_ty], [llvm_anyptr_ty], []>;

## Heap elision optimization intrinsics

def int_experimental_coro_elide : Intrinsic<[llvm_ptr_ty], [], []>;
def int_experimental_coro_delete : Intrinsic<[llvm_i1_ty], [llvm_ptr_ty], []>;

## Optimizations